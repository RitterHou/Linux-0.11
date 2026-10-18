/*
 * 文件系统：缓冲区缓存（buffer.c）、Minix v1 文件系统（fs.c）、打开的文件和文件描述符（file.c）。
 * 磁盘上的结构与 Linux 0.11 的 include/linux/fs.h 相同，tools/mkfs.py 按同样的格式生成镜像。
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>

#define BLOCK_SIZE 1024             /* Minix v1 的块大小：2 个扇区 */
#define NAME_LEN   14               /* 目录项里文件名最长 14 个字节 */
#define ROOT_INO   1                /* 根目录的 i 节点号 */

#define S_IFMT  0170000             /* i_mode 中表示文件类型的几位 */
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000             /* 字符设备：i 节点的第一个区块号存的是设备号（主设备号 << 8 | 次设备号） */
#define TTY_MAJOR 4                 /* 终端：次设备号 0 是控制台（/dev/tty0），1 是第 6 行（/dev/tty1） */

/* buffer.c：缓冲区缓存 */
struct buffer {
    uint32_t block;                 /* 装的是哪一块；NO_BLOCK 表示空 */
    uint8_t valid;                  /* 内容已经从硬盘读进来了 */
    uint8_t locked;                 /* 正在从硬盘读，别人要等 */
    uint16_t count;                 /* 有几个人正在用它；为 0 时可以换成别的块 */
    uint32_t last_used;             /* 最近一次被用到的时刻，换块时挑最久没用的 */
    uint8_t data[BLOCK_SIZE];
};
#define NO_BLOCK 0xFFFFFFFFu
struct buffer *bread(uint32_t block);
void brelse(struct buffer *b);
extern uint32_t buffer_hits, buffer_misses;

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

int fs_mount(void);
int read_inode(uint32_t ino, struct minix_inode *out);
int32_t inode_read(const struct minix_inode *ip, uint32_t pos, void *dst, uint32_t n);
int namei(const char *path, uint32_t *ino, struct minix_inode *ip);

/* file.c：打开的文件 */
#define NR_OPEN 8                   /* 每个任务最多同时打开 8 个文件 */
#define FILE_INODE 1                /* 文件系统里的文件或目录 */
#define FILE_TASKS 2                /* 第 6 行的任务输出区（/dev/tty1） */
#define FILE_CONSOLE 3              /* 控制台：键盘输入和第 8～18 行的窗口（/dev/tty0） */
struct file {
    int type;
    int count;                      /* 有几个文件描述符指向它；fork 之后父子共用，读写位置也共用 */
    uint32_t pos;
    uint32_t ino;
    struct minix_inode inode;
};
struct file *file_open(const char *path);
struct file *tasks_file(void);
void file_get(struct file *f);
void file_put(struct file *f);
int32_t file_read(struct file *f, void *dst, uint32_t n);
int32_t file_write(struct file *f, const void *src, uint32_t n);

#endif
