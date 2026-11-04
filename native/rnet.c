#include "rnet.h"
#include "build_config.h"
#include "engine.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__aarch64__) && BASALT_ARM_744
#define QUEUE_LENGTH 32
#define FILTER_MAX 64
struct packet { size_t length; uint8_t bytes[RNET_PACKET_MAX]; };
static struct packet packets[QUEUE_LENGTH];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned read_index, write_index, count;
static int capturing, generation, installed, socket_fd = -1;
static uint8_t prefix[FILTER_MAX];
static size_t prefix_length;
static struct sockaddr_storage destination;
static socklen_t destination_length;
static struct sockaddr_storage local_address;
static socklen_t local_length;

static ssize_t (*original_sendto)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
static ssize_t (*original_sendmsg)(int, const struct msghdr*, int);
static int (*original_sendmmsg)(int, struct mmsghdr*, unsigned int, int);

static int udp_destination(int fd, const struct sockaddr* address, socklen_t length,
                           struct sockaddr_storage* result, socklen_t* result_length)
{
    int kind = 0; socklen_t kind_length = sizeof(kind);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &kind, &kind_length) || kind != SOCK_DGRAM) return 0;
    if (!address) {
        *result_length = sizeof(*result);
        if (getpeername(fd, (struct sockaddr*)result, result_length)) return 0;
    } else {
        if (length < sizeof(sa_family_t) || length > sizeof(*result)) return 0;
        memcpy(result, address, length);
        *result_length = length;
    }
    return (result->ss_family == AF_INET && *result_length >= sizeof(struct sockaddr_in)) ||
           (result->ss_family == AF_INET6 && *result_length >= sizeof(struct sockaddr_in6));
}

static int blocked(const uint8_t* bytes, size_t length)
{
    pthread_mutex_lock(&lock);
    int result = prefix_length && length >= prefix_length && !memcmp(bytes, prefix, prefix_length);
    pthread_mutex_unlock(&lock);
    return result;
}

static void record(int fd, const struct sockaddr* address, socklen_t address_length,
                   const uint8_t* bytes, size_t length)
{
    struct sockaddr_storage peer = {0}; socklen_t peer_length = 0;
    if (!bytes || !length || !udp_destination(fd, address, address_length, &peer, &peer_length)) return;
    struct sockaddr_storage source = {0}; socklen_t source_length = sizeof(source);
    if (getsockname(fd, (struct sockaddr*)&source, &source_length)) return;
    pthread_mutex_lock(&lock);
    socket_fd = fd; destination = peer; destination_length = peer_length;
    local_address = source; local_length = source_length;
    if (capturing && length <= RNET_PACKET_MAX) {
        if (count == QUEUE_LENGTH) { read_index = (read_index + 1) % QUEUE_LENGTH; --count; }
        struct packet* entry = &packets[write_index];
        memcpy(entry->bytes, bytes, length); entry->length = length;
        write_index = (write_index + 1) % QUEUE_LENGTH; ++count;
    }
    pthread_mutex_unlock(&lock);
}

static ssize_t wrapped_sendto(int fd, const void* data, size_t length, int flags,
                              const struct sockaddr* address, socklen_t address_length)
{
    struct sockaddr_storage peer; socklen_t peer_length = 0;
    int udp = data && udp_destination(fd, address, address_length, &peer, &peer_length);
    if (udp && blocked(data, length)) return (ssize_t)length;
    ssize_t sent = original_sendto(fd, data, length, flags, address, address_length);
    if (udp && sent > 0) record(fd, address, address_length, data, (size_t)sent);
    return sent;
}

static int flatten(const struct msghdr* message, uint8_t* output, size_t* length)
{
    size_t total = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) {
        size_t part = message->msg_iov[i].iov_len;
        if (part > RNET_PACKET_MAX - total) return 0;
        memcpy(output + total, message->msg_iov[i].iov_base, part);
        total += part;
    }
    *length = total;
    return 1;
}

static ssize_t wrapped_sendmsg(int fd, const struct msghdr* message, int flags)
{
    struct sockaddr_storage peer; socklen_t peer_length = 0;
    int udp = message && udp_destination(fd, message->msg_name, message->msg_namelen, &peer, &peer_length);
    uint8_t bytes[RNET_PACKET_MAX]; size_t length = 0;
    int valid = udp && flatten(message, bytes, &length);
    if (valid && blocked(bytes, length)) return (ssize_t)length;
    ssize_t sent = original_sendmsg(fd, message, flags);
    if (valid && sent > 0) record(fd, message->msg_name, message->msg_namelen, bytes, (size_t)sent);
    return sent;
}

static int wrapped_sendmmsg(int fd, struct mmsghdr* messages, unsigned int count, int flags)
{
    struct sockaddr_storage peer; socklen_t peer_length = 0;
    int udp = count && messages && udp_destination(fd, messages[0].msg_hdr.msg_name,
                                                     messages[0].msg_hdr.msg_namelen, &peer, &peer_length);
    pthread_mutex_lock(&lock);
    int filtering = prefix_length != 0;
    pthread_mutex_unlock(&lock);
    if (udp && filtering) {
        unsigned int done = 0;
        for (; done < count; ++done) {
            struct msghdr* message = &messages[done].msg_hdr;
            ssize_t sent = wrapped_sendmsg(fd, message, flags);
            if (sent < 0) return done ? (int)done : -1;
            messages[done].msg_len = (unsigned int)sent;
        }
        return (int)done;
    }
    int sent = original_sendmmsg(fd, messages, count, flags);
    if (udp && sent > 0) {
        uint8_t bytes[RNET_PACKET_MAX]; size_t length;
        for (int i = 0; i < sent; ++i)
            if (flatten(&messages[i].msg_hdr, bytes, &length))
                record(fd, messages[i].msg_hdr.msg_name, messages[i].msg_hdr.msg_namelen,
                       bytes, messages[i].msg_len <= length ? messages[i].msg_len : length);
    }
    return sent;
}

struct slot { uintptr_t offset; const char* symbol; void* replacement; void* original; };
static struct slot slots[] = {
    {0x6c0dcf0, "sendmmsg", wrapped_sendmmsg, NULL},
    {0x6c0dcf8, "sendto", wrapped_sendto, NULL},
    {0x6c0de58, "sendmsg", wrapped_sendmsg, NULL},
};

static int patch_slot(struct slot* slot, void* value)
{
    uintptr_t target = g_engine.base + slot->offset;
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return 0;
    uintptr_t page = target & ~((uintptr_t)size - 1);
    if (mprotect((void*)page, (size_t)size, PROT_READ | PROT_WRITE)) return 0;
    __atomic_store_n((void**)target, value, __ATOMIC_RELEASE);
    if (!mprotect((void*)page, (size_t)size, PROT_READ)) return 1;
    return 0;
}

static int install(void)
{
    if (installed) return 1;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        struct slot* slot = &slots[i];
        void* actual = dlsym(RTLD_DEFAULT, slot->symbol);
        void* resolved = __atomic_load_n((void**)(g_engine.base + slot->offset), __ATOMIC_ACQUIRE);
        if (!actual || resolved != actual) return 0;
        slot->original = actual;
    }
    original_sendmmsg = slots[0].original;
    original_sendto = slots[1].original;
    original_sendmsg = slots[2].original;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        if (!patch_slot(&slots[i], slots[i].replacement)) {
            for (size_t j = 0; j <= i; ++j) patch_slot(&slots[j], slots[j].original);
            return 0;
        }
    }
    installed = 1;
    return 1;
}

int rnet_start(void)
{
    if (!install()) return 0;
    pthread_mutex_lock(&lock);
    capturing = 1;
    generation = generation == INT_MAX ? 1 : generation + 1;
    read_index = write_index = count = 0;
    int current = generation;
    pthread_mutex_unlock(&lock);
    return current;
}

void rnet_stop(int current)
{
    pthread_mutex_lock(&lock);
    if (current == generation) { capturing = 0; read_index = write_index = count = 0; }
    pthread_mutex_unlock(&lock);
}

int rnet_poll(int current, uint8_t* output, size_t* length)
{
    pthread_mutex_lock(&lock);
    int status = 0;
    if (current != generation || !capturing) status = -1;
    else if (count) {
        struct packet* entry = &packets[read_index];
        *length = entry->length; memcpy(output, entry->bytes, *length);
        read_index = (read_index + 1) % QUEUE_LENGTH; --count; status = 1;
    }
    pthread_mutex_unlock(&lock);
    return status;
}

int rnet_filter(const uint8_t* bytes, size_t length)
{
    if (length > FILTER_MAX || !install()) return 0;
    pthread_mutex_lock(&lock);
    if (length) memcpy(prefix, bytes, length);
    prefix_length = length;
    pthread_mutex_unlock(&lock);
    return 1;
}

int rnet_send(const uint8_t* data, size_t length)
{
    if (!length || length > 65507 || !installed) return 0;
    pthread_mutex_lock(&lock);
    int fd = socket_fd;
    struct sockaddr_storage peer = destination;
    socklen_t peer_length = destination_length;
    struct sockaddr_storage source = local_address;
    socklen_t source_length = local_length;
    pthread_mutex_unlock(&lock);
    if (fd < 0 || !peer_length) return 0;
    struct sockaddr_storage current = {0}; socklen_t current_length = sizeof(current);
    if (getsockname(fd, (struct sockaddr*)&current, &current_length) ||
        current_length != source_length || memcmp(&current, &source, current_length)) return 0;
    return original_sendto(fd, data, length, 0, (struct sockaddr*)&peer, peer_length) == (ssize_t)length;
}
#else
int rnet_start(void) { return 0; }
void rnet_stop(int generation) { (void)generation; }
int rnet_poll(int generation, uint8_t* output, size_t* length)
{ (void)generation; (void)output; (void)length; return -1; }
int rnet_filter(const uint8_t* prefix, size_t length) { (void)prefix; (void)length; return 0; }
int rnet_send(const uint8_t* data, size_t length) { (void)data; (void)length; return 0; }
#endif
