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

/*
 * 把用户给的路径复制进内核，变成绝对路径：/ 开头的原样用，否则接在当前目录后面。路径里的 . 和 .. 不用管，
 * 它们本来就是每个目录里的两项，查路径时照常查下去。太长、不在用户的内存里，返回 -1。
 * Linux 0.11 的 namei() 遇到不以 / 开头的路径，从 current->pwd 这个 i 节点开始查，道理相同。
 */
static int get_path(char *dst, uint32_t src, int from_user)
{
    char name[PATH_MAX];
    int len = copy_string(name, src, PATH_MAX, from_user);

    if (len < 0)
        return -1;
    int n = 0;
    if (name[0] != '/') {
        for (const char *p = current_cwd(); *p; p++)
            dst[n++] = *p;
        if (dst[n - 1] != '/')
            dst[n++] = '/';
    }
    if (n + len >= PATH_MAX)
        return -1;
    for (int i = 0; i <= len; i++)
        dst[n + i] = name[i];
    return 0;
}

/* open(path, flags)：打开文件，返回最小的空闲描述符。flags 见 syscall.h 的 O_ 开头的几个。 */
static int32_t sys_open(uint32_t path, uint32_t flags, uint32_t c, struct interrupt_frame *frame)
{
    (void)c;
    char name[PATH_MAX];
    struct file **files = current_files();

    if (get_path(name, path, from_user_mode(frame)) < 0)
        return -1;
    int fd = free_fd();
    if (fd < 0)
        return -1;
    struct file *f = file_open(name, (int)flags);
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

    if (!from_user_mode(frame) || get_path(name, path, 1) < 0)
        return -1;
    fs_lock();                      /* 装入程序时一直读文件，不能让别人在中途改它 */
    int ret = do_exec(name, argv, frame);
    fs_unlock();
    return ret;
}

/* stat(path, &st)：取文件的类型、链接数、大小和 i 节点号。内核要写 st，检查它是否用户可写。 */
static int32_t sys_stat(uint32_t path, uint32_t st, uint32_t c, struct interrupt_frame *frame)
{
    (void)c;
    char name[PATH_MAX];
    uint32_t ino;
    struct minix_inode inode;
    int from_user = from_user_mode(frame);

    if (get_path(name, path, from_user) < 0 || (from_user && !user_write_ok(st, sizeof(struct stat))))
        return -1;
    fs_lock();
    int found = namei(name, &ino, &inode);
    fs_unlock();
    if (found < 0)
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

/* unlink、mkdir、rmdir 都是：先把路径复制进内核，在文件系统锁里交给 fs.c。 */
static int32_t path_op(uint32_t path, struct interrupt_frame *frame, int (*op)(const char *))
{
    char name[PATH_MAX];

    if (get_path(name, path, from_user_mode(frame)) < 0)
        return -1;
    fs_lock();
    int ret = op(name);
    fs_unlock();
    return ret;
}

static int32_t sys_unlink(uint32_t path, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c;
    return path_op(path, frame, fs_unlink);
}

static int32_t sys_mkdir(uint32_t path, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c;
    return path_op(path, frame, fs_mkdir);
}

static int32_t sys_rmdir(uint32_t path, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c;
    return path_op(path, frame, fs_rmdir);
}

/* fsinfo(&info)：数一数两张位图里还有几个 0。先取到内核里，再复制给用户。 */
static int32_t sys_fsinfo(uint32_t info, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c;
    struct fsinfo tmp;

    if (from_user_mode(frame) && !user_write_ok(info, sizeof(tmp)))
        return -1;
    fs_lock();
    int ok = fs_info(&tmp);
    fs_unlock();
    if (ok < 0)
        return -1;
    *(struct fsinfo *)info = tmp;
    return 0;
}

/*
 * 把绝对路径就地规范化：去掉多余的 /、. 和 ..，例如 /bin/../etc/./ 变成 /etc。没有符号链接，
 * 按字面去掉 .. 和真的往上走一级是一回事。
 */
static void normalize(char *path)
{
    char out[PATH_MAX];
    int n = 0;
    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        int len = (int)(p - start);
        if (len == 0 || (len == 1 && start[0] == '.'))
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;                /* 连同前面的 / 一起去掉 */
            continue;
        }
        out[n++] = '/';
        for (int i = 0; i < len; i++)
            out[n++] = start[i];
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = '\0';
    for (int i = 0; i <= n; i++)
        path[i] = out[i];
}

/*
 * chdir(path)：换当前目录。必须是存在的目录。
 * Linux 0.11 的 current->pwd 是目录的 i 节点，这里记的是规范化后的路径：getcwd 直接复制出来就行
 * （Linux 0.11 没有 getcwd，pwd 要沿着 .. 一级级往上找名字）。代价是当前目录可以被 rmdir 掉，
 * 之后用相对路径什么也找不到；Linux 0.11 的 rmdir 见 i 节点还被别人引用（i_count > 1）就不删。
 */
static int32_t sys_chdir(uint32_t path, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c;
    char name[PATH_MAX];
    uint32_t ino;
    struct minix_inode d;

    if (get_path(name, path, from_user_mode(frame)) < 0)
        return -1;
    fs_lock();
    int found = namei(name, &ino, &d);
    fs_unlock();
    if (found < 0 || (d.mode & S_IFMT) != S_IFDIR)
        return -1;
    normalize(name);
    char *cwd = current_cwd();
    for (int i = 0; i < PATH_MAX; i++)
        cwd[i] = name[i];
    return 0;
}

/* getcwd(buf, size)：把当前目录写进 buf，返回长度；放不下返回 -1。 */
static int32_t sys_getcwd(uint32_t buf, uint32_t size, uint32_t c, struct interrupt_frame *frame)
{
    (void)c;
    const char *cwd = current_cwd();
    uint32_t len = 0;

    while (cwd[len])
        len++;
    if (len + 1 > size || (from_user_mode(frame) && !user_write_ok(buf, len + 1)))
        return -1;
    for (uint32_t i = 0; i <= len; i++)
        ((char *)buf)[i] = cwd[i];
    return (int32_t)len;
}

/* kill(pid, sig)：发信号，见 sched_kill()。 */
static int32_t sys_kill(uint32_t pid, uint32_t sig, uint32_t c, struct interrupt_frame *frame)
{
    (void)c, (void)frame;
    return sched_kill((int)pid, (int)sig);
}

/* signal(sig, handler, restorer)：返回原来的处理方式。 */
static int32_t sys_signal(uint32_t sig, uint32_t handler, uint32_t restorer, struct interrupt_frame *frame)
{
    (void)frame;
    return sched_signal((int)sig, handler, restorer);
}

/* setsid()：自己当一个新的进程组的头，返回组号。 */
static int32_t sys_setsid(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)a, (void)b, (void)c, (void)frame;
    return sched_setsid();
}

/* brk(addr)：挪堆的末尾，见 sched_brk()。 */
static int32_t sys_brk(uint32_t addr, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    return (int32_t)sched_brk(addr);
}

/* alarm(seconds)：定闹钟，返回上一个还剩几秒，见 sched_alarm()。 */
static int32_t sys_alarm(uint32_t seconds, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    return (int32_t)sched_alarm(seconds);
}

/* pause()：等一个信号，返回 -EINTR。 */
static int32_t sys_pause(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)a, (void)b, (void)c, (void)frame;
    return sched_pause();
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
    [SYS_UNLINK] = sys_unlink,
    [SYS_MKDIR]  = sys_mkdir,
    [SYS_RMDIR]  = sys_rmdir,
    [SYS_FSINFO] = sys_fsinfo,
    [SYS_CHDIR]  = sys_chdir,
    [SYS_GETCWD] = sys_getcwd,
    [SYS_KILL]   = sys_kill,
    [SYS_SIGNAL] = sys_signal,
    [SYS_SETSID] = sys_setsid,
    [SYS_BRK]    = sys_brk,
    [SYS_ALARM]  = sys_alarm,
    [SYS_PAUSE]  = sys_pause,
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
