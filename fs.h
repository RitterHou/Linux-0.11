/*
 * 文件系统：缓冲区缓存（buffer.c）、Minix v1 文件系统（fs.c）、打开的文件和文件描述符（file.c）。
 * 磁盘上的结构与 Linux 0.11 的 include/linux/fs.h 相同，tools/mkfs.py 按同样的格式生成镜像。
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "kernel.h"

#define BLOCK_SIZE 1024             /* Minix v1 的块大小：2 个扇区 */
#define NAME_LEN   14               /* 目录项里文件名最长 14 个字节 */
#define ROOT_INO   1                /* 根目录的 i 节点号 */

#define S_IFMT  0170000             /* i_mode 中表示文件类型的几位 */
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000             /* 字符设备：i 节点的第一个区块号存的是设备号（主设备号 << 8 | 次设备号） */
#define S_IFBLK 0060000             /* 块设备（硬盘）：同上，mount 用它找到是哪块盘 */
#define TTY_MAJOR 4                 /* 终端：次设备号 0 是控制台（/dev/tty0），1 是第 6 行（/dev/tty1），64 是串口（/dev/ttyS0） */

/* buffer.c：缓冲区缓存 */
/*
 * 设备号：主设备号 << 8 | 次设备号，与 Linux 0.11 相同。硬盘的主设备号是 3，次设备号 0 是第一块盘（hda）整块，
 * 5 是第二块（hdb）整块（1～4、6～9 是两块盘上的分区，这里不分区）。根文件系统在 hda 上。
 */
#define HD_MAJOR 3
#define ROOT_DEV (HD_MAJOR << 8 | 0)
#define HDB_DEV  (HD_MAJOR << 8 | 5)

struct buffer {
    uint32_t dev;                   /* 哪块盘的 */
    uint32_t block;                 /* 装的是哪一块；NO_BLOCK 表示空 */
    uint8_t valid;                  /* 内容已经从硬盘读进来了 */
    uint8_t locked;                 /* 正在从硬盘读，别人要等 */
    uint16_t count;                 /* 有几个人正在用它；为 0 时可以换成别的块 */
    uint8_t dirty;                  /* 改过了，还没写回硬盘 */
    uint32_t last_used;             /* 最近一次被用到的时刻，换块时挑最久没用的 */
    uint8_t data[BLOCK_SIZE];
};
#define NO_BLOCK 0xFFFFFFFFu
#define NR_BUFFERS 16               /* 缓冲区缓存有几个缓冲区 */
struct buffer *bread(uint32_t dev, uint32_t block);
struct buffer *bget(uint32_t dev, uint32_t block);
int bflush_dev(uint32_t dev);
void bdirty(struct buffer *b);
int bsync(void);
uint32_t buffer_dirty(void);
void brelse(struct buffer *b);
extern uint32_t buffer_hits, buffer_misses, buffer_writes;

/* fs.c：Minix v1 文件系统，只读 */
struct minix_inode {                /* 磁盘上的 i 节点，32 字节 */
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t time;
    uint8_t gid;
    uint8_t nlinks;
    uint16_t zone[9];               /* 7 个直接块、1 个间接块、1 个二级间接块 */
} __attribute__((packed));

struct dir_entry {                  /* 目录项，16 字节 */
    uint16_t inode;
    char name[NAME_LEN];
} __attribute__((packed));

/* 内存里的 i 节点：同一个文件打开几次，共用表里的一项 */
struct m_inode {
    uint32_t dev;                   /* 在哪块盘上 */
    uint32_t ino;
    int count;                      /* 有几处在用：打开的文件、正在执行的操作 */
    struct minix_inode d;           /* 磁盘上那 32 字节的拷贝，改了就写回去 */
};

/* permission() 的 mask：要读、写、执行（目录是查找）哪几样 */
#define MAY_READ  4
#define MAY_WRITE 2
#define MAY_EXEC  1
int permission(const struct minix_inode *ip, int mask);

int fs_mount(void);
int read_inode(uint32_t dev, uint32_t ino, struct minix_inode *out);
int32_t inode_read(uint32_t dev, const struct minix_inode *ip, uint32_t pos, void *dst, uint32_t n);
int namei(const char *path, uint32_t *dev, uint32_t *ino, struct minix_inode *ip);
void fs_lock(void);
void fs_unlock(void);
struct m_inode *iget(uint32_t dev, uint32_t ino);
void iput(struct m_inode *ip);
int32_t inode_write(struct m_inode *ip, uint32_t pos, const void *src, uint32_t n);
void fs_truncate(struct m_inode *ip);
struct m_inode *fs_create(const char *path);
int fs_mkdir(const char *path);
int fs_unlink(const char *path);
int fs_link(const char *oldpath, const char *newpath);
int fs_chmod(const char *path, uint32_t mode);
int fs_chown(const char *path, uint32_t uid, uint32_t gid);
int fs_utime(const char *path, uint32_t t);
int fs_rmdir(const char *path);
struct fsinfo;
int fs_info(uint32_t n, struct fsinfo *info);
int fs_do_mount(const char *devpath, const char *dirpath, const char *on);
int fs_do_umount(const char *path);

/* file.c：打开的文件 */
#define NR_OPEN 8                   /* 每个任务最多同时打开 8 个文件 */
#define FILE_INODE 1                /* 文件系统里的文件或目录 */
#define FILE_TASKS 2                /* 第 6 行的任务输出区（/dev/tty1） */
#define FILE_TTY   3                /* 终端：控制台 /dev/tty0（键盘和第 8～18 行的窗口）或串口 /dev/ttyS0，见 f->tty */
#define FILE_PIPE_READ  4           /* 管道读的一头 */
#define FILE_PIPE_WRITE 5           /* 管道写的一头 */
struct file {
    int type;
    int count;                      /* 有几个文件描述符指向它；fork 之后父子共用，读写位置也共用 */
    int mode;                       /* 打开方式：O_RDONLY、O_WRONLY 或 O_RDWR，再加上 O_APPEND */
    uint32_t pos;
    struct m_inode *ip;             /* 文件系统里的文件：内存里的 i 节点 */
    struct pipe *pipe;              /* 管道的两头指向同一个管道 */
    int tty;                        /* 终端：TTY_CONSOLE 或 TTY_SERIAL */
};
struct file *file_open(const char *path, int flags);
int file_pipe(struct file **read_end, struct file **write_end);
struct file *tasks_file(void);
void file_get(struct file *f);
void file_put(struct file *f);
int32_t file_read(struct file *f, void *dst, uint32_t n);
int32_t file_write(struct file *f, const void *src, uint32_t n);

/* pipe.c：管道 */
struct pipe {
    uint8_t *buf;                   /* 一页的缓冲区 */
    uint32_t head, tail;            /* 只增不减：head - tail 是管道里的字节数 */
    int readers, writers;           /* 读、写两头各被几个打开的文件拿着 */
    struct wait_queue wait;
};
struct pipe *pipe_new(void);
void pipe_close(struct pipe *p, int writer);
int32_t pipe_read(struct pipe *p, void *dst, uint32_t n);
int32_t pipe_write(struct pipe *p, const void *src, uint32_t n);

#endif
