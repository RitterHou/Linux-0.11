/*
 * Minix v1 文件系统，只读。与 Linux 0.11 的 fs/super.c、inode.c、namei.c 中读文件的部分对应。
 *
 *   块 0：引导块（不用）  块 1：超级块  然后是 i 节点位图、区块位图、i 节点表，最后是数据区块。
 *   每个 i 节点 32 字节，有 9 个区块号：前 7 个直接指向数据块，第 8 个指向一个间接块（里面是 512 个区块号），
 *   第 9 个指向二级间接块（512 个间接块的块号）。目录也是文件，内容是一条条 16 字节的目录项。
 */
#include "kernel.h"
#include "fs.h"

#define SUPER_MAGIC 0x137F           /* Minix v1，文件名 14 个字节 */
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(struct minix_inode))
#define ZONES_PER_BLOCK  (BLOCK_SIZE / 2)

static struct {                     /* 磁盘上的超级块 */
    uint16_t ninodes;
    uint16_t nzones;
    uint16_t imap_blocks;
    uint16_t zmap_blocks;
    uint16_t firstdatazone;
    uint16_t log_zone_size;
    uint32_t max_size;
    uint16_t magic;
} __attribute__((packed)) sb;

static int mounted;                 /* 0：还没挂载；1：已挂载；-1：硬盘上不是 Minix 文件系统 */

/*
 * 第一次用到文件系统时读超级块，检查魔数，在第 1 行显示。只能在任务里调用（读硬盘要睡）。
 * Linux 0.11 由 init 进程调用 setup 系统调用，再在 mount_root() 里读根文件系统的超级块。
 */
int fs_mount(void)
{
    if (mounted)
        return mounted > 0 ? 0 : -1;
    struct buffer *b = bread(1);
    if (!b) {
        mounted = -1;
        return -1;
    }
    uint8_t *dst = (uint8_t *)&sb;
    for (uint32_t i = 0; i < sizeof(sb); i++)
        dst[i] = b->data[i];
    brelse(b);
    if (sb.magic != SUPER_MAGIC || sb.log_zone_size != 0) {   /* 区块比块大的格式不支持 */
        mounted = -1;
        return -1;
    }
    mounted = 1;

    struct text_line line;
    line_init(&line);
    line_append(&line, "minix fs: ");
    line_append_dec(&line, sb.ninodes);
    line_append(&line, " inodes, ");
    line_append_dec(&line, sb.nzones);
    line_append(&line, " blocks");
    vga_write_at(1, 46, line.text, VGA_GRAY);
    return 0;
}

/* 读第 ino 个 i 节点。i 节点表紧跟在两张位图后面。 */
int read_inode(uint32_t ino, struct minix_inode *out)
{
    if (ino == 0 || ino > sb.ninodes)
        return -1;
    uint32_t block = 2 + sb.imap_blocks + sb.zmap_blocks + (ino - 1) / INODES_PER_BLOCK;
    struct buffer *b = bread(block);
    if (!b)
        return -1;
    const uint8_t *src = b->data + (ino - 1) % INODES_PER_BLOCK * sizeof(struct minix_inode);
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++)
        dst[i] = src[i];
    brelse(b);
    return 0;
}

/* 间接块 zone 里的第 i 个区块号。 */
static uint32_t indirect(uint32_t zone, uint32_t i)
{
    if (zone == 0)
        return 0;
    struct buffer *b = bread(zone);
    if (!b)
        return 0;
    uint32_t value = ((uint16_t *)b->data)[i];
    brelse(b);
    return value;
}

/* 文件的第 n 块在磁盘上是哪一块；0 表示文件在这里没有块（读出来是 0）。Linux 0.11 的 bmap() 也是这样分三段。 */
static uint32_t bmap(const struct minix_inode *ip, uint32_t n)
{
    if (n < 7)
        return ip->zone[n];
    n -= 7;
    if (n < ZONES_PER_BLOCK)
        return indirect(ip->zone[7], n);
    n -= ZONES_PER_BLOCK;
    if (n < ZONES_PER_BLOCK * ZONES_PER_BLOCK)
        return indirect(indirect(ip->zone[8], n / ZONES_PER_BLOCK), n % ZONES_PER_BLOCK);
    return 0;
}

/*
 * 从文件的第 pos 个字节起读最多 n 个字节到 dst，返回读到的字节数（到了文件末尾就少读），出错返回 -1。
 * dst 可以是用户的缓冲区：复制时可能缺页或写时复制，由缺页异常处理，这时拿着缓冲区也没关系。
 */
int32_t inode_read(const struct minix_inode *ip, uint32_t pos, void *dst, uint32_t n)
{
    uint8_t *out = dst;
    uint32_t done = 0;

    if (pos >= ip->size)
        return 0;
    if (n > ip->size - pos)
        n = ip->size - pos;
    while (done < n) {
        uint32_t at = pos + done, off = at % BLOCK_SIZE, chunk = BLOCK_SIZE - off;
        if (chunk > n - done)
            chunk = n - done;
        uint32_t zone = bmap(ip, at / BLOCK_SIZE);
        if (zone == 0) {
            for (uint32_t i = 0; i < chunk; i++)
                out[done + i] = 0;
        } else {
            struct buffer *b = bread(zone);
            if (!b)
                return -1;
            for (uint32_t i = 0; i < chunk; i++)
                out[done + i] = b->data[off + i];
            brelse(b);
        }
        done += chunk;
    }
    return (int32_t)done;
}

/* 在目录 dir 里找名字为 name（长 len）的项，返回它的 i 节点号；找不到返回 0。 */
static uint32_t lookup(const struct minix_inode *dir, const char *name, int len)
{
    struct dir_entry de;

    if ((dir->mode & S_IFMT) != S_IFDIR)
        return 0;
    for (uint32_t pos = 0; pos + sizeof(de) <= dir->size; pos += sizeof(de)) {
        if (inode_read(dir, pos, &de, sizeof(de)) != sizeof(de))
            return 0;
        if (de.inode == 0)
            continue;
        int i = 0;
        while (i < len && de.name[i] == name[i])
            i++;
        if (i == len && (len == NAME_LEN || de.name[len] == '\0'))
            return de.inode;
    }
    return 0;
}

/*
 * 按路径找到文件：从根目录起，一段一段地在目录里查下去。成功时返回 0，并给出 i 节点号和 i 节点。
 * 还没有“当前目录”，只认 / 开头的绝对路径。Linux 0.11 的 namei() 还处理当前目录、符号链接之外的种种情况。
 */
int namei(const char *path, uint32_t *ino, struct minix_inode *ip)
{
    if (fs_mount() < 0 || path[0] != '/')
        return -1;
    uint32_t n = ROOT_INO;
    if (read_inode(n, ip) < 0)
        return -1;
    const char *p = path;
    for (;;) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        int len = 0;
        while (p[len] && p[len] != '/')
            len++;
        if (len > NAME_LEN)
            return -1;
        n = lookup(ip, p, len);
        if (n == 0 || read_inode(n, ip) < 0)
            return -1;
        p += len;
    }
    *ino = n;
    return 0;
}
