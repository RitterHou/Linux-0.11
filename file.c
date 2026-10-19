/*
 * 打开的文件：open 在文件表里占一项，记下 i 节点和读写位置；每个任务的文件描述符表（sched.c）指向这些项。
 * fork 之后父子进程的描述符指向同一项，读写位置也共用，这是 Unix 的规矩：
 * 父进程先读掉的部分，子进程接着往后读。Linux 0.11 的 file_table[NR_FILE] 和 current->filp[] 也是这样。
 * 第 6 行的任务输出区也当成一个“文件”，开机时由内核放在每个任务的 1 号描述符上。
 * 打开字符设备文件（/dev/tty0、/dev/tty1）得到的也是这样的文件，读写时按类型交给终端或第 6 行；
 * 管道的两头也各是一项，读写交给 pipe.c。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"
#include "syscall.h"

#define NR_FILE 32

static struct file file_table[NR_FILE];
static struct file tasks_row = { .type = FILE_TASKS, .count = 1 };  /* 永远不会被释放 */

struct file *tasks_file(void)
{
    return &tasks_row;
}

/* 在文件表里占一项，类型为 type；满了返回 0。 */
static struct file *file_alloc(int type)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < NR_FILE; i++) {
        struct file *f = &file_table[i];
        if (f->count == 0) {
            *f = (struct file){ .type = type, .count = 1 };
            irq_restore(flags);
            return f;
        }
    }
    irq_restore(flags);
    return 0;
}

/*
 * 打开 path，返回文件表里的一项。flags 是 O_RDONLY / O_WRONLY / O_RDWR，可以再加上：
 * O_CREAT（不存在就新建）、O_EXCL（和 O_CREAT 一起：已经存在就失败）、O_TRUNC（清空）、O_APPEND（接在末尾写）。
 * 文件不存在、目录要打开来写、设备不认识、文件表满了，返回 0。Linux 0.11 的 open_namei() 做的是同样的判断。
 */
struct file *file_open(const char *path, int flags)
{
    uint32_t dev, ino;
    struct minix_inode d;
    struct m_inode *ip = 0;
    int access = flags & O_ACCMODE, type = FILE_INODE, tty = 0;

    fs_lock();
    if (namei(path, &dev, &ino, &d) == 0) {
        /* 读要有读权限，写和清空要有写权限：权限只在打开时查一次，打开以后 chmod 也不影响已经打开的描述符 */
        int want = (access != O_WRONLY ? MAY_READ : 0) | (access != O_RDONLY || (flags & O_TRUNC) ? MAY_WRITE : 0);
        if (((flags & O_CREAT) && (flags & O_EXCL)) || !permission(&d, want))
            goto fail;
        if ((d.mode & S_IFMT) == S_IFCHR) {
            if (d.zone[0] == (TTY_MAJOR << 8 | 0) || (d.zone[0] == (TTY_MAJOR << 8 | 64) && serial_present())) {
                type = FILE_TTY;
                tty = d.zone[0] == (TTY_MAJOR << 8 | 0) ? TTY_CONSOLE : TTY_SERIAL;
                if (sched_claim_tty(tty))
                    tty_set_pgrp(tty, sched_pgrp());    /* 进程组的头第一次打开终端：它这一组成了这个终端的前台进程组 */
            } else if (d.zone[0] == (TTY_MAJOR << 8 | 1)) {
                type = FILE_TASKS;
            } else {
                goto fail;          /* 没有这个设备（没有串口时的 /dev/ttyS0 也是） */
            }
        } else {
            if (((d.mode & S_IFMT) == S_IFDIR && access != O_RDONLY) || (d.mode & S_IFMT) == S_IFBLK)
                goto fail;          /* 目录只有内核能改；硬盘这样的块设备只能 mount，不能直接读写 */
            if (!(ip = iget(dev, ino)))
                goto fail;
            if ((flags & O_TRUNC) && access != O_RDONLY)
                fs_truncate(ip);
        }
    } else if (!(flags & O_CREAT) || !(ip = fs_create(path))) {
        goto fail;
    }
    struct file *f = file_alloc(type);
    if (!f) {
        iput(ip);
        goto fail;
    }
    f->ip = ip;
    f->tty = tty;
    f->mode = flags & (O_ACCMODE | O_APPEND);
    fs_unlock();
    return f;
fail:
    fs_unlock();
    return 0;
}

/* 建一个管道，得到读写两头两个打开的文件。文件表满了或者没有内存时返回 -1，什么也不留下。 */
int file_pipe(struct file **read_end, struct file **write_end)
{
    struct pipe *p = pipe_new();
    if (!p)
        return -1;
    struct file *r = file_alloc(FILE_PIPE_READ), *w = r ? file_alloc(FILE_PIPE_WRITE) : 0;
    if (!w) {
        if (r)
            r->count = 0;
        free_page((uint32_t)p->buf);
        p->buf = 0;
        return -1;
    }
    r->pipe = w->pipe = p;
    p->readers = p->writers = 1;
    *read_end = r;
    *write_end = w;
    return 0;
}

void file_get(struct file *f)
{
    uint32_t flags = irq_save();
    f->count++;
    irq_restore(flags);
}

/*
 * 关掉一个打开的文件：引用次数减到 0，管道就关掉这一头，文件系统里的文件就放下它的 i 节点
 * （iput 可能要收回已经删掉的文件的块，要读写硬盘，所以在文件系统锁里、开着中断做）。
 * i 节点先在关中断时取出来：引用次数一到 0，这一项随时可能被别的 open 拿去用。
 */
void file_put(struct file *f)
{
    uint32_t flags = irq_save();
    if (f->count <= 0)
        panic("trying to close a closed file");
    int last = --f->count == 0;
    struct m_inode *ip = last && f->type == FILE_INODE ? f->ip : 0;
    if (last && (f->type == FILE_PIPE_READ || f->type == FILE_PIPE_WRITE))
        pipe_close(f->pipe, f->type == FILE_PIPE_WRITE);   /* 最后一个拿着这一头的关掉了 */
    irq_restore(flags);
    if (ip) {
        fs_lock();
        iput(ip);
        fs_unlock();
    }
}

int32_t file_read(struct file *f, void *dst, uint32_t n)
{
    if (n == 0)
        return 0;
    if (f->type == FILE_TTY)
        return tty_read(f->tty, dst, n);
    if (f->type == FILE_PIPE_READ)
        return pipe_read(f->pipe, dst, n);
    if (f->type != FILE_INODE || (f->mode & O_ACCMODE) == O_WRONLY)
        return -1;
    /*
     * 在文件系统锁里读：读硬盘会睡，fork 之后共用这个文件的另一个进程可能同时在读写，
     * 先占下要读的这一段、把位置往后移，再去读，两边就不会读到同一段。
     */
    fs_lock();
    uint32_t start = f->pos, size = f->ip->d.size;
    uint32_t left = size > start ? size - start : 0;
    if (n > left)
        n = left;
    f->pos += n;
    int32_t got = inode_read(f->ip->dev, &f->ip->d, start, dst, n);
    fs_unlock();
    return got;
}

int32_t file_write(struct file *f, const void *src, uint32_t n)
{
    const char *p = src;

    if (f->type == FILE_TTY)
        return tty_write(f->tty, src, n);
    if (f->type == FILE_PIPE_WRITE)
        return pipe_write(f->pipe, src, n);
    if (f->type == FILE_INODE) {
        if ((f->mode & O_ACCMODE) == O_RDONLY || (f->ip->d.mode & S_IFMT) != S_IFREG)
            return -1;
        fs_lock();
        if (f->mode & O_APPEND)
            f->pos = f->ip->d.size;
        int32_t done = inode_write(f->ip, f->pos, src, n);
        if (done > 0)
            f->pos += (uint32_t)done;
        fs_unlock();
        return done;
    }
    if (f->type != FILE_TASKS)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        task_write_char(p[i]);
    return (int32_t)n;
}
