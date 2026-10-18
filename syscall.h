/*
 * 系统调用的编号和调用方式，内核和用户程序共用。
 *
 * 调用方执行 int 0x80：EAX 是调用号，EBX、ECX、EDX 依次是第 1～3 个参数，返回值放回 EAX。
 * 这与 Linux 0.11 的约定相同，见 main 分支的 include/unistd.h 中的 _syscall3 等宏。
 * 失败时返回 -1（Linux 返回负的错误码，这里简化为 -1）。
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

#define SYS_WRITE  0                /* write(fd, buf, count)：fd 1 是第 6 行的任务输出区 */
#define SYS_GETPID 1                /* getpid()：返回当前任务号 */
#define SYS_SLEEP  2                /* sleep(ticks)：等待若干个定时器 tick */
#define SYS_EXIT   3                /* exit(code)：结束当前任务，不再返回 */
#define SYS_FORK   4                /* fork()：复制当前任务；父进程得到新任务号，子进程得到 0 */
#define SYS_CALLS  5

static inline int32_t syscall3(uint32_t nr, uint32_t a, uint32_t b, uint32_t c)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

static inline int32_t write(int fd, const void *buf, uint32_t count)
{
    return syscall3(SYS_WRITE, (uint32_t)fd, (uint32_t)buf, count);
}

static inline int32_t getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

static inline int32_t sleep(uint32_t ticks)
{
    return syscall3(SYS_SLEEP, ticks, 0, 0);
}

static inline int32_t fork(void)
{
    return syscall3(SYS_FORK, 0, 0, 0);
}

static inline void exit(int code)
{
    syscall3(SYS_EXIT, (uint32_t)code, 0, 0);
    for (;;)                        /* 内核不会让它返回；这个循环告诉编译器 exit 不会返回 */
        ;
}

#endif
