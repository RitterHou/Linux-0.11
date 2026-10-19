/*
 * 系统调用的编号和调用方式，内核和用户程序共用。
 *
 * 调用方执行 int 0x80：EAX 是调用号，EBX、ECX、EDX 依次是第 1～3 个参数，返回值放回 EAX。
 * 这与 Linux 0.11 的约定相同，见 main 分支的 include/unistd.h 中的 _syscall3 等宏。
 * 失败时返回 -1（Linux 返回负的错误码，这里简化为 -1）；只有睡着等的时候被信号打断，返回 -EINTR，和 Linux 相同。
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

#define SYS_WRITE  0                /* write(fd, buf, count)：开机时 fd 1 是第 6 行的任务输出区 */
#define SYS_GETPID 1                /* getpid()：返回当前任务号 */
#define SYS_SLEEP  2                /* sleep(ticks)：等待若干个定时器 tick */
#define SYS_EXIT   3                /* exit(code)：结束当前任务，不再返回 */
#define SYS_FORK   4                /* fork()：复制当前任务；父进程得到新任务号，子进程得到 0 */
#define SYS_WAITPID 5               /* waitpid(pid, &status, options)：等子进程结束，返回它的任务号 */
#define SYS_OPEN   6                /* open(path, flags)：打开文件，返回文件描述符 */
#define SYS_READ   7                /* read(fd, buf, count)：读文件，返回读到的字节数，到末尾返回 0 */
#define SYS_CLOSE  8                /* close(fd)：关闭文件描述符 */
#define SYS_EXEC   9                /* exec(path, argv)：换成磁盘上的程序；成功时不返回 */
#define SYS_STAT   10               /* stat(path, &st)：取文件的类型、大小、i 节点号 */
#define SYS_TASKINFO 11             /* taskinfo(pid, &info)：取任务的状态、父进程和名字，给 ps 用 */
#define SYS_PIPE   12               /* pipe(fd)：建一个管道，fd[0] 是读的一头，fd[1] 是写的一头 */
#define SYS_DUP    13               /* dup(fd)：复制描述符，返回最小的空闲描述符，指向同一个打开的文件 */
#define SYS_UNLINK 14               /* unlink(path)：删掉文件 */
#define SYS_MKDIR  15               /* mkdir(path)：新建目录 */
#define SYS_RMDIR  16               /* rmdir(path)：删掉空目录 */
#define SYS_FSINFO 17               /* fsinfo(&info)：文件系统有多少块、多少 i 节点、各用了多少，给 df 用 */
#define SYS_CHDIR  18               /* chdir(path)：换当前目录 */
#define SYS_GETCWD 19               /* getcwd(buf, size)：取当前目录的绝对路径 */
#define SYS_KILL   20               /* kill(pid, sig)：给任务（pid > 0）或进程组（0 是自己的组，负数是 -pid 那一组）发信号 */
#define SYS_SIGNAL 21               /* signal(sig, handler, restorer)：这个信号来了怎么办：结束进程、不理，或者调用 handler */
#define SYS_SETSID 22               /* setsid()：自己当一个新的进程组的头，返回组号 */
#define SYS_BRK    23               /* brk(addr)：把堆的末尾挪到 addr，返回现在的堆末尾；brk(0) 只是问一下 */
#define SYS_ALARM  24               /* alarm(seconds)：几秒之后给自己发 SIGALRM；0 是取消 */
#define SYS_PAUSE  25               /* pause()：睡到来了一个信号为止 */
#define SYS_CALLS  26

/* fsinfo 写回的内容 */
struct fsinfo {
    uint32_t blocks, free_blocks;   /* 数据块 */
    uint32_t inodes, free_inodes;
};

/* stat 写回的内容：取自磁盘上的 i 节点 */
struct stat {
    uint16_t mode;                  /* 类型和权限：0040000 目录，0100000 普通文件，0020000 字符设备 */
    uint16_t nlinks;
    uint32_t size;
    uint32_t ino;
};

/* taskinfo 写回的内容 */
#define TASK_NAME_LEN 16
struct taskinfo {
    int32_t state;                  /* 1 能运行，2 在 sleep 里，3 僵尸，4 在 waitpid 里，5 在等待队列上 */
    int32_t parent;
    char name[TASK_NAME_LEN];
};

#define WNOHANG    1                /* waitpid 的选项：子进程还没结束就立刻返回 0 */
#define EINTR      4                /* 系统调用在睡着等的时候被信号打断，返回 -EINTR */

/* 信号的编号与 Linux 0.11 的 include/signal.h 相同 */
#define SIGINT     2                /* 终端上按了 Ctrl+C */
#define SIGKILL    9                /* 不能忽略 */
#define SIGPIPE    13               /* 往读的一头全关了的管道里写 */
#define SIGALRM    14               /* alarm 定的闹钟到了 */
#define SIGTERM    15               /* kill 命令默认发的信号 */
typedef void (*sighandler_t)(int);
#define SIG_DFL    ((sighandler_t)0)    /* 默认动作：结束进程 */
#define SIG_IGN    ((sighandler_t)1)    /* 不理这个信号 */
#define SIG_ERR    ((sighandler_t)-1)   /* signal 失败 */

/* open 的 flags，取值与 Linux 0.11 的 include/fcntl.h 相同 */
#define O_RDONLY   00
#define O_WRONLY   01
#define O_RDWR     02
#define O_ACCMODE  03
#define O_CREAT    0100             /* 不存在就新建 */
#define O_EXCL     0200             /* 和 O_CREAT 一起用：已经存在就失败 */
#define O_TRUNC    01000            /* 打开时清空 */
#define O_APPEND   02000            /* 每次写都接在末尾 */

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

static inline int32_t open(const char *path, int32_t flags)
{
    return syscall3(SYS_OPEN, (uint32_t)path, (uint32_t)flags, 0);
}

static inline int32_t read(int fd, void *buf, uint32_t count)
{
    return syscall3(SYS_READ, (uint32_t)fd, (uint32_t)buf, count);
}

static inline int32_t close(int fd)
{
    return syscall3(SYS_CLOSE, (uint32_t)fd, 0, 0);
}

static inline int32_t execv(const char *path, char *const argv[])
{
    return syscall3(SYS_EXEC, (uint32_t)path, (uint32_t)argv, 0);
}

static inline int32_t stat(const char *path, struct stat *st)
{
    return syscall3(SYS_STAT, (uint32_t)path, (uint32_t)st, 0);
}

static inline int32_t taskinfo(int32_t pid, struct taskinfo *info)
{
    return syscall3(SYS_TASKINFO, (uint32_t)pid, (uint32_t)info, 0);
}

static inline int32_t pipe(int32_t fd[2])
{
    return syscall3(SYS_PIPE, (uint32_t)fd, 0, 0);
}

static inline int32_t dup(int32_t fd)
{
    return syscall3(SYS_DUP, (uint32_t)fd, 0, 0);
}

static inline int32_t unlink(const char *path)
{
    return syscall3(SYS_UNLINK, (uint32_t)path, 0, 0);
}

static inline int32_t mkdir(const char *path)
{
    return syscall3(SYS_MKDIR, (uint32_t)path, 0, 0);
}

static inline int32_t rmdir(const char *path)
{
    return syscall3(SYS_RMDIR, (uint32_t)path, 0, 0);
}

static inline int32_t fsinfo(struct fsinfo *info)
{
    return syscall3(SYS_FSINFO, (uint32_t)info, 0, 0);
}

static inline int32_t chdir(const char *path)
{
    return syscall3(SYS_CHDIR, (uint32_t)path, 0, 0);
}

static inline int32_t getcwd(char *buf, uint32_t size)
{
    return syscall3(SYS_GETCWD, (uint32_t)buf, size, 0);
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

static inline int32_t waitpid(int32_t pid, int32_t *status, int32_t options)
{
    return syscall3(SYS_WAITPID, (uint32_t)pid, (uint32_t)status, (uint32_t)options);
}

static inline int32_t kill(int32_t pid, int32_t sig)
{
    return syscall3(SYS_KILL, (uint32_t)pid, (uint32_t)sig, 0);
}

/*
 * 处理函数返回到 sig_restore（用户程序自己的代码，见 user/ulib.c）：它恢复内核在栈上保存的寄存器，
 * 回到被信号打断的地方。Linux 0.11 的 libc 也是把 __sig_restore 的地址当第三个参数交给 sys_signal。
 */
void sig_restore(void);

static inline sighandler_t signal(int32_t sig, sighandler_t handler)
{
    return (sighandler_t)syscall3(SYS_SIGNAL, (uint32_t)sig, (uint32_t)handler, (uint32_t)sig_restore);
}

static inline uint32_t brk(uint32_t addr)
{
    return (uint32_t)syscall3(SYS_BRK, addr, 0, 0);
}

static inline uint32_t alarm(uint32_t seconds)
{
    return (uint32_t)syscall3(SYS_ALARM, seconds, 0, 0);
}

static inline int32_t pause(void)
{
    return syscall3(SYS_PAUSE, 0, 0, 0);
}

static inline int32_t setsid(void)
{
    return syscall3(SYS_SETSID, 0, 0, 0);
}

static inline __attribute__((noreturn)) void exit(int code)
{
    syscall3(SYS_EXIT, (uint32_t)code, 0, 0);
    for (;;)                        /* 内核不会让它返回；这个循环告诉编译器 exit 不会返回 */
        ;
}

#endif
