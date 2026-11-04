#include "http_bridge.h"
#include "engine.h"
#include "device_http.h"
#include "build_config.h"
#include "vm_layout.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#if BASALT_DEV_BRIDGE
#include <android/log.h>
#endif

#if BASALT_DEV_BRIDGE
#define HTTP_HOST "10.0.2.2"
#define HTTP_PORT 17981
#endif
#define HTTP_MAX_ENVELOPE (6u * 8u * 1024u * 1024u + 64u * 1024u)
#define HTTP_MAX_JOBS 64

typedef struct { uint64_t value; uint32_t extra, tag; } TValue;
struct job {
    uint32_t id;
    int state; // 0 free, 1 pending, 2 complete
    int cancelled;
    char* request;
    size_t request_size;
    char* response;
    size_t response_size;
    char error[192];
    time_t completed_at;
};

static struct job jobs[HTTP_MAX_JOBS];
static uint32_t next_id;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
#if defined(__x86_64__) || (defined(BASALT_ARM_744) && BASALT_ARM_744)
static TValue* base(void* L) { return *(TValue**)((uint8_t*)L + 0x58); }
static TValue* top(void* L) { return *(TValue**)((uint8_t*)L + 0x70); }
#define L_TOP_OFFSET 0x70
#else
static TValue* base(void* L) { return *(TValue**)((uint8_t*)L + 0x28); }
static TValue* top(void* L) { return *(TValue**)((uint8_t*)L + 0x20); }
#define L_TOP_OFFSET 0x20
#endif

static void push(void* L, TValue value)
{
    if (!g_engine.check_stack(L, 1)) return;
#if !defined(__x86_64__)
    if (*((uint8_t*)L + VM_L_GC_FLAGS) & 4) g_engine.thread_barrier(L, L, (uint8_t*)L + VM_L_GC_LIST);
#endif
    *top(L) = value;
    *(TValue**)((uint8_t*)L + L_TOP_OFFSET) = top(L) + 1;
}
static void push_string(void* L, const char* bytes, size_t length)
{
#if defined(__x86_64__)
    push(L, (TValue){.value=(uintptr_t)g_engine.new_string(L, bytes, length), .tag=6});
#else
    g_engine.lua_pushlstring(L, bytes, length);
#endif
}
static void push_boolean(void* L, int value)
{
#if defined(__x86_64__)
    push(L, (TValue){.value=value != 0, .tag=1});
#else
    g_engine.lua_pushboolean(L, value);
#endif
}
static void push_number(void* L, double number)
{
    TValue result = {.tag=3};
    memcpy(&result.value, &number, 8);
    push(L, result);
}
static int failure(void* L, const char* message)
{
    push(L, (TValue){0});
    push_string(L, message, strlen(message));
    return 2;
}
static int poll_failure(void* L, const char* message)
{
    push_boolean(L, 1);
    push(L, (TValue){0});
    push_string(L, message, strlen(message));
    return 3;
}

#if BASALT_DEV_BRIDGE
static int connect_host(char* error, size_t size)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { snprintf(error, size, "socket: %s", strerror(errno)); return -1; }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(HTTP_PORT);
    inet_pton(AF_INET, HTTP_HOST, &address.sin_addr);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int status = connect(fd, (struct sockaddr*)&address, sizeof(address));
    if (status != 0 && errno == EINPROGRESS)
    {
        struct pollfd ready = {.fd=fd, .events=POLLOUT};
        if (poll(&ready, 1, 3000) > 0 && (ready.revents & POLLOUT))
        {
            int code = 0;
            socklen_t n = sizeof(code);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &n);
            status = code ? -1 : 0;
            if (code) errno = code;
        }
        else { status = -1; errno = ETIMEDOUT; }
    }
    if (status != 0)
    { snprintf(error, size, "host bridge unavailable: %s", strerror(errno)); close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    struct timeval timeout = {.tv_sec=18, .tv_usec=0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

static int send_all(int fd, const char* bytes, size_t length)
{
    while (length)
    {
        ssize_t n = send(fd, bytes, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n;
        length -= n;
    }
    return 0;
}

// Explicit development fixtures only; real traffic stays on-device.
static int host_only(const char* request, size_t length)
{
    const char* const needles[] = {"127.0.0.1:17982"};
    for (size_t i = 0; i < sizeof(needles) / sizeof(*needles); ++i)
    {
        size_t size = strlen(needles[i]);
        for (size_t at = 0; at + size <= length; ++at)
            if (!memcmp(request + at, needles[i], size)) return 1;
    }
    return 0;
}
#endif

static void* relay(void* parameter)
{
    struct job* task = parameter;
    char error[192] = {0};
    char* buffer = NULL;
    size_t used = 0, body_length = 0;
#if BASALT_DEV_BRIDGE
    if (!host_only(task->request, task->request_size))
#endif
    {
        if (!device_http_request(task->request, task->request_size, &buffer, &used, error, sizeof(error)))
        {
            pthread_mutex_lock(&lock);
            free(task->request);
            task->request = NULL;
            if (task->cancelled) { free(buffer); memset(task, 0, sizeof(*task)); }
            else
            {
                task->response = buffer;
                task->response_size = used;
                task->completed_at = time(NULL);
                task->state = 2;
            }
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        // A real HTTP failure (TLS error, unreachable URL, etc.) should be
        // returned directly, not retried through an unrelated desktop relay.
        if (error[0]) goto finish;
    }
#if BASALT_DEV_BRIDGE
    int fd = connect_host(error, sizeof(error));
    if (fd >= 0)
    {
        char header[256];
        int n = snprintf(header, sizeof(header),
                         "POST /http HTTP/1.0\r\nHost: " HTTP_HOST "\r\n"
                         "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         task->request_size);
        if (n <= 0 || n >= (int)sizeof(header) || send_all(fd, header, (size_t)n) ||
            send_all(fd, task->request, task->request_size))
            snprintf(error, sizeof(error), "could not send request to host bridge");
        else
        {
            const size_t max = HTTP_MAX_ENVELOPE + 16384u;
            size_t capacity = 65536u;
            buffer = malloc(capacity + 1);
            if (!buffer) snprintf(error, sizeof(error), "out of memory");
            else for (;;)
            {
                if (used == max) { snprintf(error, sizeof(error), "HTTP response envelope exceeds limit"); break; }
                if (used == capacity)
                {
                    size_t next = capacity > max / 2 ? max : capacity * 2;
                    char* larger = realloc(buffer, next + 1);
                    if (!larger) { snprintf(error, sizeof(error), "out of memory"); break; }
                    buffer = larger;
                    capacity = next;
                }
                ssize_t read_count = recv(fd, buffer + used, capacity - used, 0);
                if (read_count < 0 && errno == EINTR) continue;
                if (read_count < 0) { snprintf(error, sizeof(error), "HTTP receive: %s", strerror(errno)); break; }
                if (read_count == 0) break;
                used += (size_t)read_count;
            }
        }
        close(fd);
    }
    char* body = NULL;
    if (!error[0] && buffer)
    {
        buffer[used] = 0;
        char* sep = strstr(buffer, "\r\n\r\n");
        if (!sep || strncmp(buffer, "HTTP/1.", 7))
            snprintf(error, sizeof(error), "invalid bridge response");
        else
        {
            body = sep + 4;
            body_length = used - (size_t)(body - buffer);
            if (body_length > HTTP_MAX_ENVELOPE)
                snprintf(error, sizeof(error), "HTTP response envelope exceeds limit");
            else
                memmove(buffer, body, body_length);
        }
    }
#else
    snprintf(error, sizeof(error), "Android HTTP transport unavailable");
#endif
finish:
    pthread_mutex_lock(&lock);
    free(task->request);
    task->request = NULL;
    if (task->cancelled)
    {
        free(buffer);
        memset(task, 0, sizeof(*task));
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    if (error[0])
    {
        free(buffer);
        snprintf(task->error, sizeof(task->error), "%s", error);
    }
    else
    {
        task->response = buffer;
        task->response_size = body_length;
    }
    task->completed_at = time(NULL);
    task->state = 2;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static int request_start(void* L)
{
    TValue* args = base(L);
    if (top(L) <= args || args[0].tag != 6)
        return failure(L, "request expects a JSON-encoded table");
    uintptr_t value = args[0].value;
    uint32_t length = *(uint32_t*)(value + 0x14);
    if (!length || length > HTTP_MAX_ENVELOPE) return failure(L, "request envelope exceeds limit");
    pthread_mutex_lock(&lock);
    struct job* task = NULL;
    for (int i = 0; i < HTTP_MAX_JOBS; ++i)
    {
        if (jobs[i].state == 2 && time(NULL) - jobs[i].completed_at > 120)
        { free(jobs[i].response); memset(&jobs[i], 0, sizeof(jobs[i])); }
        if (!task && !jobs[i].state) task = &jobs[i];
    }
    if (!task)
    {
#if BASALT_DEV_BRIDGE
        int pending = 0, completed = 0;
        for (int i = 0; i < HTTP_MAX_JOBS; ++i)
            if (jobs[i].state == 1) ++pending;
            else if (jobs[i].state == 2) ++completed;
        __android_log_print(ANDROID_LOG_WARN, "BasaltHTTP", "queue full: pending=%d completed=%d", pending, completed);
#endif
        pthread_mutex_unlock(&lock);
        return failure(L, "too many pending HTTP requests");
    }
    task->request = malloc(length);
    if (!task->request) { pthread_mutex_unlock(&lock); return failure(L, "out of memory"); }
    memcpy(task->request, (const void*)(value + 0x18), length);
    task->request_size = length;
    task->id = ++next_id;
    if (!task->id) task->id = ++next_id;
    task->state = 1;
    pthread_t thread;
    int code = pthread_create(&thread, NULL, relay, task);
    if (code)
    {
        free(task->request);
        memset(task, 0, sizeof(*task));
        pthread_mutex_unlock(&lock);
        return failure(L, "could not start HTTP worker");
    }
    pthread_detach(thread);
    unsigned id = task->id;
    pthread_mutex_unlock(&lock);
    push_number(L, id);
    return 1;
}

static int request_poll(void* L)
{
    TValue* args = base(L);
    if (top(L) <= args || args[0].tag != 3)
        return poll_failure(L, "request poll expects an ID");
    double id_number;
    memcpy(&id_number, &args[0].value, 8);
    if (id_number < 1 || id_number > UINT32_MAX || id_number != (uint32_t)id_number)
        return poll_failure(L, "invalid HTTP request ID");
    pthread_mutex_lock(&lock);
    struct job* task = NULL;
    for (int i = 0; i < HTTP_MAX_JOBS; ++i)
        if (jobs[i].state && jobs[i].id == (uint32_t)id_number) { task = &jobs[i]; break; }
    if (!task) { pthread_mutex_unlock(&lock); return poll_failure(L, "HTTP request ID expired"); }
    if (task->state == 1)
    {
        pthread_mutex_unlock(&lock);
        push_boolean(L, 0);
        return 1;
    }
    char* response = task->response;
    size_t length = task->response_size;
    char error[sizeof(task->error)];
    memcpy(error, task->error, sizeof(error));
    memset(task, 0, sizeof(*task));
    pthread_mutex_unlock(&lock);
    push_boolean(L, 1);
    if (response)
    {
        push_string(L, response, length);
        free(response);
        return 2;
    }
    push(L, (TValue){0});
    push_string(L, error, strlen(error));
    return 3;
}

static int request_await(void* L)
{
    TValue* args = base(L);
    if (top(L) <= args || args[0].tag != 3) return poll_failure(L, "request wait expects an ID");
    double number;
    memcpy(&number, &args[0].value, sizeof(number));
    if (number < 1 || number > UINT32_MAX || number != (uint32_t)number)
        return poll_failure(L, "invalid HTTP request ID");
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 30;
    pthread_mutex_lock(&lock);
    for (;;)
    {
        struct job* task = NULL;
        for (int i = 0; i < HTTP_MAX_JOBS; ++i)
            if (jobs[i].state && jobs[i].id == (uint32_t)number) { task = &jobs[i]; break; }
        if (!task || task->state == 2) break;
        if (pthread_cond_timedwait(&changed, &lock, &deadline) == ETIMEDOUT)
        {
            task->cancelled = 1;
            pthread_mutex_unlock(&lock);
            return poll_failure(L, "HTTP request timed out");
        }
    }
    pthread_mutex_unlock(&lock);
    return request_poll(L);
}

static int request_cancel(void* L)
{
    TValue* args = base(L);
    if (top(L) <= args || args[0].tag != 3) return 0;
    double number;
    memcpy(&number, &args[0].value, sizeof(number));
    if (number < 1 || number > UINT32_MAX || number != (uint32_t)number) return 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < HTTP_MAX_JOBS; ++i)
    {
        struct job* task = &jobs[i];
        if (!task->state || task->id != (uint32_t)number) continue;
        if (task->state == 1) task->cancelled = 1;
        else { free(task->response); memset(task, 0, sizeof(*task)); }
        break;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

static void register_one(void* L, void* env, const char* name, int (*func)(void*))
{
    g_engine.lua_pushcclosurek(L, func, name, 0, NULL);
    TValue key = {.value=(uintptr_t)g_engine.new_string(L, name, strlen(name)), .tag=6};
    TValue closure = top(L)[-1];
    TValue* slot = g_engine.table_set(L, env, &key);
    *slot = closure;
    if ((*((uint8_t*)env + 1) & 4) && (*((uint8_t*)closure.value + 1) & 3))
        g_engine.table_barrier(L, env, (void*)closure.value);
    *(TValue**)((uint8_t*)L + L_TOP_OFFSET) = top(L) - 1;
}

void register_http_bridge(void* L, void* env)
{
    register_one(L, env, "_requestStart", request_start);
    register_one(L, env, "_requestPoll", request_poll);
    register_one(L, env, "_requestCancel", request_cancel);
    register_one(L, env, "_requestAwait", request_await);
}
#if defined(__x86_64__)
int x86_http_request_start(void* L) { return request_start(L); }
int x86_http_request_poll(void* L) { return request_poll(L); }
#endif
