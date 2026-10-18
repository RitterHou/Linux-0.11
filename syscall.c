/*
 * 系统调用的内核一侧。
 *
 * int 0x80 经 IDT 第 0x80 项（DPL 3 的陷阱门）进入 isr.asm 的 isr_syscall，
 * 和其他中断一样整理好栈后交给 interrupt_dispatch()，再转到这里的 syscall_dispatch()：
 * 按 EAX 中的调用号查表，把 EBX、ECX、EDX 作为参数，返回值写回栈上保存的 EAX，
 * 中断返回时 popad 恢复寄存器，调用方就在 EAX 里拿到了返回值。
 * Linux 0.11 的 kernel/system_call.s 用同样的方式查 sys_call_table。
 */
#include "kernel.h"
#include "fs.h"
#include "syscall.h"

/*
 * 前三个参数是调用方放在 EBX、ECX、EDX 里的值。最后一个参数是栈上保存的现场：
 * 从它的 CS 看出调用方是否在用户态（用户态传来的指针要检查），fork 还要复制它。
 */
typedef int32_t (*syscall_fn)(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame);

/* 当前任务的第 fd 号描述符指向的文件；fd 不对或者没打开返回 0。 */
static struct file *fd_file(uint32_t fd)
{
    return fd < NR_OPEN ? current_files()[fd] : 0;
}

/* 当前任务最小的空闲描述符；用光了返回 -1。 */
static int free_fd(void)
{
    struct file **files = current_files();
    for (int fd = 0; fd < NR_OPEN; fd++)
        if (!files[fd])
            return fd;
    return -1;
}

/*
 * write(fd, buf, count)：写到 fd 指向的文件。目前能写的只有第 6 行的任务输出区。
 * 用户态传来的 buf 必须完全落在用户程序的内存里：内核能读所有内存，不检查的话，
 * 用户程序把内核地址交给 write，就能让内核替它把内核内存印出来。
 * Linux 0.11 不需要这一步：内核经 FS（用户数据段）用 get_fs_byte() 读用户内存，
 * 用户给的指针按用户段解释，越界由段界限挡住。这里的段都是平坦的，只能显式检查。
 */
static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t count, struct interrupt_frame *frame)
{
    struct file *f = fd_file(fd);

    if (!f || (from_user_mode(frame) && !user_range_ok(buf, count)))
        return -1;
    return file_write(f, (const void *)buf, count);
}

/* read(fd, buf, count)：从 fd 指向的文件读，返回读到的字节数，到了文件末尾返回 0。内核要写 buf，所以检查它是否用户可写。 */
static int32_t sys_read(uint32_t fd, uint32_t buf, uint32_t count, struct interrupt_frame *frame)
{
    struct file *f = fd_file(fd);

    if (!f || (from_user_mode(frame) && !user_write_ok(buf, count)))
        return -1;
    return file_read(f, (void *)buf, count);
}

/* open(path, flags)：打开文件，返回最小的空闲描述符。文件系统只读，flags 暂时不用。 */
static int32_t sys_open(uint32_t path, uint32_t flags, uint32_t c, struct interrupt_frame *frame)
{
    (void)flags, (void)c;
    char name[PATH_MAX];
    struct file **files = current_files();

    if (copy_string(name, path, PATH_MAX, from_user_mode(frame)) < 0)
        return -1;
    int fd = free_fd();
    if (fd < 0)
        return -1;
    struct file *f = file_open(name);
    if (!f)
        return -1;
    files[fd] = f;
    return fd;
}

/* close(fd)：关掉描述符。fork 出的另一方可能还在用这个文件，引用次数减到 0 才真正关掉。 */
static int32_t sys_close(uint32_t fd, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    struct file *f = fd_file(fd);

    if (!f)
        return -1;
    current_files()[fd] = 0;
    file_put(f);
    return 0;
}

static int32_t sys_getpid(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)a, (void)b, (void)c, (void)frame;
    return current_pid();
}

/* sleep(ticks)：睡 ticks 个 tick，期间把 CPU 让给别的任务，见 sched.c 的 sched_sleep()。 */
static int32_t sys_sleep(uint32_t ticks, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    sched_sleep(ticks);
    return 0;
}

/* exit(code)：在第 7 行说明当前任务以什么代码结束，然后结束它，不再返回。 */
static int32_t sys_exit(uint32_t code, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    struct text_line line;

    line_init(&line);
    line_append(&line, "task ");
    line_append_dec(&line, (uint32_t)current_pid());
    line_append(&line, " exited with code ");
    line_append_dec(&line, code);
    line_show(&line, VGA_YELLOW, TASK_MESSAGE_ROW);
    sched_exit(code & 0xFF);        /* 退出码只留低 8 位，与 Unix 相同 */
}

/* fork()：只有用户任务能 fork，内核任务 A 没有用户态的现场可以复制。 */
static int32_t sys_fork(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)a, (void)b, (void)c;
    return from_user_mode(frame) ? sched_fork(frame) : -1;
}

/*
 * waitpid(pid, status, options)：等子进程结束，见 sched.c 的 sched_waitpid()。
 * status 可以为 0（不要状态）；不为 0 时内核要往那里写 4 个字节，用户传来的地址必须落在用户可写的内存里。
 */
static int32_t sys_waitpid(uint32_t pid, uint32_t status, uint32_t options, struct interrupt_frame *frame)
{
    if (status != 0 && from_user_mode(frame) && !user_write_ok(status, 4))
        return -1;
    return sched_waitpid((int32_t)pid, (uint32_t *)status, (int)options);
}

/* exec(path, argv)：换成磁盘上的程序，见 exec.c。成功时不返回到调用它的程序，而是从新程序的入口开始执行。 */
static int32_t sys_exec(uint32_t path, uint32_t argv, uint32_t c, struct interrupt_frame *frame)
{
    (void)c;
    char name[PATH_MAX];

    if (!from_user_mode(frame) || copy_string(name, path, PATH_MAX, 1) < 0)
        return -1;
    return do_exec(name, argv, frame);
}

/* stat(path, &st)：取文件的类型、链接数、大小和 i 节点号。内核要写 st，检查它是否用户可写。 */
static int32_t sys_stat(uint32_t path, uint32_t st, uint32_t c, struct interrupt_frame *frame)
{
    (void)c;
    char name[PATH_MAX];
    uint32_t ino;
    struct minix_inode inode;
    int from_user = from_user_mode(frame);

    if (copy_string(name, path, PATH_MAX, from_user) < 0 || (from_user && !user_write_ok(st, sizeof(struct stat))))
        return -1;
    if (namei(name, &ino, &inode) < 0)
        return -1;
    struct stat *out = (struct stat *)st;
    out->mode = inode.mode;
    out->nlinks = inode.nlinks;
    out->size = inode.size;
    out->ino = ino;
    return 0;
}

/* taskinfo(pid, &info)：见 sched.c 的 sched_taskinfo()。先取到内核里，再复制给用户。 */
static int32_t sys_taskinfo(uint32_t pid, uint32_t info, uint32_t c, struct interrupt_frame *frame)
{
    (void)c;
    struct taskinfo tmp;

    if (from_user_mode(frame) && !user_write_ok(info, sizeof(tmp)))
        return -1;
    if (sched_taskinfo((int)pid, &tmp) < 0)
        return -1;
    *(struct taskinfo *)info = tmp;
    return 0;
}

/*
 * pipe(fd)：建一个管道，读的一头放进最小的空闲描述符，写的一头放进下一个，写回 fd[0]、fd[1]。
 * 描述符不够、文件表满了或者没有内存时返回 -1，什么也不留下。内核要写 fd，检查它是否用户可写。
 */
static int32_t sys_pipe(uint32_t fds, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c;
    struct file **files = current_files();
    struct file *r, *w;

    if (from_user_mode(frame) && !user_write_ok(fds, 8))
        return -1;
    int rfd = free_fd();
    if (rfd < 0 || file_pipe(&r, &w) < 0)
        return -1;
    files[rfd] = r;
    int wfd = free_fd();
    if (wfd < 0) {
        files[rfd] = 0;
        file_put(r);
        file_put(w);
        return -1;
    }
    files[wfd] = w;
    ((int32_t *)fds)[0] = rfd;
    ((int32_t *)fds)[1] = wfd;
    return 0;
}

/* dup(fd)：最小的空闲描述符也指向 fd 指向的那个打开的文件。shell 用 close(1); dup(管道写的一头); 把输出接到管道上。 */
static int32_t sys_dup(uint32_t fd, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    struct file *f = fd_file(fd);
    int nfd = free_fd();

    if (!f || nfd < 0)
        return -1;
    file_get(f);
    current_files()[nfd] = f;
    return nfd;
}

static const syscall_fn syscall_table[SYS_CALLS] = {
    [SYS_WRITE]  = sys_write,
    [SYS_GETPID] = sys_getpid,
    [SYS_SLEEP]  = sys_sleep,
    [SYS_EXIT]   = sys_exit,
    [SYS_FORK]   = sys_fork,
    [SYS_WAITPID] = sys_waitpid,
    [SYS_OPEN]   = sys_open,
    [SYS_READ]   = sys_read,
    [SYS_CLOSE]  = sys_close,
    [SYS_EXEC]   = sys_exec,
    [SYS_STAT]   = sys_stat,
    [SYS_TASKINFO] = sys_taskinfo,
    [SYS_PIPE]   = sys_pipe,
    [SYS_DUP]    = sys_dup,
};

void syscall_dispatch(struct interrupt_frame *frame)
{
    static int user_call_seen;
    uint32_t nr = frame->eax;
    int from_user = from_user_mode(frame);

    if (from_user && !user_call_seen) {
        user_call_seen = 1;
        vga_write_at(0, 8, "U", VGA_GRAY);  /* 第一次收到来自特权级 3 的系统调用 */
    }

    if (nr >= SYS_CALLS || syscall_table[nr] == 0) {
        frame->eax = (uint32_t)-1;  /* 未知的调用号 */
        return;
    }
    frame->eax = (uint32_t)syscall_table[nr](frame->ebx, frame->ecx, frame->edx, frame);
}
