/*
 * 打开的文件：open 在文件表里占一项，记下 i 节点和读写位置；每个任务的文件描述符表（sched.c）指向这些项。
 * fork 之后父子进程的描述符指向同一项，读写位置也共用，这是 Unix 的规矩：
 * 父进程先读掉的部分，子进程接着往后读。Linux 0.11 的 file_table[NR_FILE] 和 current->filp[] 也是这样。
 * 第 6 行的任务输出区也当成一个“文件”，开机时由内核放在每个任务的 1 号描述符上。
 * 打开字符设备文件（/dev/tty0、/dev/tty1）得到的也是这样的文件，读写时按类型交给终端或第 6 行。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"

#define NR_FILE 32

static struct file file_table[NR_FILE];
static struct file tasks_row = { .type = FILE_TASKS, .count = 1 };  /* 永远不会被释放 */

struct file *tasks_file(void)
{
    return &tasks_row;
}

/* 打开 path，返回文件表里的一项；找不到文件或文件表满了返回 0。 */
struct file *file_open(const char *path)
{
    uint32_t ino;
    struct minix_inode inode;

    if (namei(path, &ino, &inode) < 0)
        return 0;
    int type = FILE_INODE;
    if ((inode.mode & S_IFMT) == S_IFCHR) {
        if (inode.zone[0] == (TTY_MAJOR << 8 | 0))
            type = FILE_CONSOLE;
        else if (inode.zone[0] == (TTY_MAJOR << 8 | 1))
            type = FILE_TASKS;
        else
            return 0;               /* 没有这个设备 */
    }
    uint32_t flags = irq_save();
    for (int i = 0; i < NR_FILE; i++) {
        struct file *f = &file_table[i];
        if (f->count == 0) {
            *f = (struct file){ .type = type, .count = 1, .pos = 0, .ino = ino, .inode = inode };
            irq_restore(flags);
            return f;
        }
    }
    irq_restore(flags);
    return 0;
}

void file_get(struct file *f)
{
    uint32_t flags = irq_save();
    f->count++;
    irq_restore(flags);
}

void file_put(struct file *f)
{
    uint32_t flags = irq_save();
    if (f->count <= 0)
        panic("trying to close a closed file");
    f->count--;
    irq_restore(flags);
}

int32_t file_read(struct file *f, void *dst, uint32_t n)
{
    if (f->type == FILE_CONSOLE)
        return tty_read(dst, n);
    if (f->type != FILE_INODE)
        return -1;
    int32_t got = inode_read(&f->inode, f->pos, dst, n);
    if (got > 0)
        f->pos += (uint32_t)got;
    return got;
}

int32_t file_write(struct file *f, const void *src, uint32_t n)
{
    const char *p = src;

    if (f->type == FILE_CONSOLE)
        return tty_write(src, n);
    if (f->type != FILE_TASKS)      /* 文件系统是只读的 */
        return -1;
    for (uint32_t i = 0; i < n; i++)
        task_write_char(p[i]);
    return (int32_t)n;
}
