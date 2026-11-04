#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/uio.h>
#include <unistd.h>

static inline int process_memory_read(uintptr_t address, void* output, size_t length)
{
    static int proc_denied;
    if (!address) return 0;
    if (!__atomic_load_n(&proc_denied, __ATOMIC_RELAXED))
    {
        int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            ssize_t n = pread(fd, output, length, (off_t)address);
            close(fd);
            if (n == (ssize_t)length) return 1;
        }
        else if (errno == EACCES || errno == EPERM)
            __atomic_store_n(&proc_denied, 1, __ATOMIC_RELAXED);
    }
    struct iovec local = {.iov_base=output, .iov_len=length};
    struct iovec remote = {.iov_base=(void*)address, .iov_len=length};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)length;
}
