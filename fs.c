/*
 * Minix v1 文件系统，与 Linux 0.11 的 fs/super.c、inode.c、namei.c、bitmap.c、truncate.c 对应。
 *
 *   块 0：引导块（不用）  块 1：超级块  然后是 i 节点位图、区块位图、i 节点表，最后是数据区块。
 *   每个 i 节点 32 字节，有 9 个区块号：前 7 个直接指向数据块，第 8 个指向一个间接块（里面是 512 个区块号），
 *   第 9 个指向二级间接块（512 个间接块的块号）。目录也是文件，内容是一条条 16 字节的目录项。
 *
 * 写的部分：
 *   - 内存里的 i 节点表（iget/iput）：同一个文件打开几次，共用表里的一项，大小和块号才不会各看各的；
 *     改了就马上写回磁盘上的 i 节点表。链接数减到 0、又没人打开着时，才真正收回它的块和 i 节点。
 *   - 两张位图：i 节点位图和区块位图里的每一位表示一个 i 节点或数据块用了没有，分配就是找一个 0 改成 1。
 *   - 写文件时，文件的第 n 块还没有就分配一块（必要时连间接块一起分配），新块先清零。
 *   - 目录项：添加时找一个空项（i 节点号为 0），没有就接在目录末尾；删除就是把 i 节点号清 0。
 * 所有会改文件系统的操作，以及读文件、查路径，都要先拿到文件系统锁 fs_lock()：读写硬盘会睡，
 * 同时只让一个任务在里面，就不会两个任务分到同一块，或者一个在读目录、另一个正改到一半。
 * Linux 0.11 给每个 i 节点、每个缓冲区各加一把锁，并发更好，道理相同。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"
#include "syscall.h"

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

/* ------------------------------------------------------------------ 文件系统锁 */

static int fs_busy;
static struct wait_queue fs_wait;

void fs_lock(void)
{
    uint32_t flags = irq_save();
    while (fs_busy)
        sleep_on(&fs_wait);
    fs_busy = 1;
    irq_restore(flags);
}

void fs_unlock(void)
{
    uint32_t flags = irq_save();
    fs_busy = 0;
    wake_up(&fs_wait);
    irq_restore(flags);
}

/* ------------------------------------------------------------------ 内存里的 i 节点表 */

#define NR_INODE 32
static struct m_inode inode_table[NR_INODE];

/* 把内存里的 i 节点写回磁盘上的 i 节点表。 */
static int write_inode(struct m_inode *ip)
{
    uint32_t block = 2 + sb.imap_blocks + sb.zmap_blocks + (ip->ino - 1) / INODES_PER_BLOCK;
    struct buffer *b = bread(block);
    if (!b)
        return -1;
    uint8_t *dst = b->data + (ip->ino - 1) % INODES_PER_BLOCK * sizeof(struct minix_inode);
    const uint8_t *src = (const uint8_t *)&ip->d;
    for (uint32_t i = 0; i < sizeof(ip->d); i++)
        dst[i] = src[i];
    bdirty(b);
    brelse(b);
    return 0;
}

/* 取第 ino 个 i 节点：表里已经有就共用，引用次数加一；没有就从磁盘读进一个空项。失败返回 0。 */
struct m_inode *iget(uint32_t ino)
{
    struct m_inode *empty = 0;

    for (int i = 0; i < NR_INODE; i++) {
        if (inode_table[i].count > 0 && inode_table[i].ino == ino) {
            inode_table[i].count++;
            return &inode_table[i];
        }
        if (inode_table[i].count == 0 && !empty)
            empty = &inode_table[i];
    }
    if (!empty || read_inode(ino, &empty->d) < 0)
        return 0;
    empty->ino = ino;
    empty->count = 1;
    return empty;
}

static void truncate_inode(struct m_inode *ip);
static void free_bit(uint32_t first_block, uint32_t bit);

/* 用完一个 i 节点。最后一个用的人放手、它的链接数又是 0（文件已经删了），才收回块和 i 节点本身。 */
void iput(struct m_inode *ip)
{
    if (!ip)
        return;
    if (--ip->count > 0)
        return;
    if (ip->d.nlinks == 0) {
        truncate_inode(ip);
        ip->d.mode = 0;
        write_inode(ip);
        free_bit(2, ip->ino);                   /* i 节点位图从块 2 开始，第 ino 位就是第 ino 个 i 节点 */
    }
}

/* ------------------------------------------------------------------ 位图 */

/* 在从 first_block 起的 nblocks 块位图里找一个为 0 的位（第 1 到 limit 位），改成 1，返回它；没有返回 0。 */
static uint32_t alloc_bit(uint32_t first_block, uint32_t nblocks, uint32_t limit)
{
    for (uint32_t blk = 0; blk < nblocks; blk++) {
        struct buffer *b = bread(first_block + blk);
        if (!b)
            return 0;
        for (uint32_t i = 0; i < BLOCK_SIZE * 8; i++) {
            uint32_t bit = blk * BLOCK_SIZE * 8 + i;
            if (bit == 0 || bit > limit)
                continue;
            if (!(b->data[i / 8] & (1 << (i % 8)))) {
                b->data[i / 8] |= (uint8_t)(1 << (i % 8));
                bdirty(b);
                brelse(b);
                return bit;
            }
        }
        brelse(b);
    }
    return 0;
}

static void free_bit(uint32_t first_block, uint32_t bit)
{
    struct buffer *b = bread(first_block + bit / (BLOCK_SIZE * 8));
    if (!b)
        return;
    uint32_t i = bit % (BLOCK_SIZE * 8);
    b->data[i / 8] &= (uint8_t)~(1 << (i % 8));
    bdirty(b);
    brelse(b);
}

/* 分配一个数据块，清零后返回它的块号；满了返回 0。区块位图的第 k 位对应第 firstdatazone + k - 1 块。 */
static uint32_t alloc_block(void)
{
    uint32_t bit = alloc_bit(2 + sb.imap_blocks, sb.zmap_blocks, sb.nzones - sb.firstdatazone);
    if (bit == 0)
        return 0;
    uint32_t block = sb.firstdatazone + bit - 1;
    struct buffer *b = bget(block);
    if (!b)
        return 0;
    bdirty(b);                      /* 清零了的新块：以后写回去，免得硬盘上留着这一块原来的内容 */
    brelse(b);
    return block;
}

static void free_block(uint32_t block)
{
    if (block >= sb.firstdatazone && block < sb.nzones)
        free_bit(2 + sb.imap_blocks, block - sb.firstdatazone + 1);
}

/* ------------------------------------------------------------------ 写文件 */

/* 间接块 zone 里的第 i 项；create 时没有就分配一块填进去。 */
static uint32_t indirect_slot(uint32_t zone, uint32_t i, int create)
{
    struct buffer *b = bread(zone);
    if (!b)
        return 0;
    uint16_t *slots = (uint16_t *)b->data;
    uint32_t value = slots[i];
    if (value == 0 && create) {
        value = alloc_block();
        if (value) {
            slots[i] = (uint16_t)value;
            bdirty(b);
        }
    }
    brelse(b);
    return value;
}

/* i 节点里的第 i 个块号；create 时没有就分配。 */
static uint32_t zone_slot(struct m_inode *ip, int i, int create)
{
    if (ip->d.zone[i] == 0 && create) {
        ip->d.zone[i] = (uint16_t)alloc_block();
        if (ip->d.zone[i])
            write_inode(ip);
    }
    return ip->d.zone[i];
}

/* 和 bmap() 一样换算文件的第 n 块，没有就分配（连同需要的间接块）。分配不了返回 0。 */
static uint32_t bmap_create(struct m_inode *ip, uint32_t n)
{
    if (n < 7)
        return zone_slot(ip, (int)n, 1);
    n -= 7;
    if (n < ZONES_PER_BLOCK) {
        uint32_t ind = zone_slot(ip, 7, 1);
        return ind ? indirect_slot(ind, n, 1) : 0;
    }
    n -= ZONES_PER_BLOCK;
    if (n < ZONES_PER_BLOCK * ZONES_PER_BLOCK) {
        uint32_t dind = zone_slot(ip, 8, 1);
        uint32_t ind = dind ? indirect_slot(dind, n / ZONES_PER_BLOCK, 1) : 0;
        return ind ? indirect_slot(ind, n % ZONES_PER_BLOCK, 1) : 0;
    }
    return 0;
}

/*
 * 从第 pos 个字节起写 n 个字节，返回写进去的字节数；一个都写不进去（磁盘满了）返回 -1。
 * 写过了文件末尾就把文件变长。src 可以是用户的缓冲区：系统调用已经把那几页准备好了。
 */
int32_t inode_write(struct m_inode *ip, uint32_t pos, const void *src, uint32_t n)
{
    const uint8_t *in = src;
    uint32_t done = 0;

    while (done < n) {
        uint32_t at = pos + done, off = at % BLOCK_SIZE, chunk = BLOCK_SIZE - off;
        if (chunk > n - done)
            chunk = n - done;
        uint32_t zone = bmap_create(ip, at / BLOCK_SIZE);
        if (zone == 0)
            break;
        struct buffer *b = bread(zone);
        if (!b)
            break;
        for (uint32_t i = 0; i < chunk; i++)
            b->data[off + i] = in[done + i];
        bdirty(b);
        brelse(b);
        done += chunk;
    }
    if (pos + done > ip->d.size) {
        ip->d.size = pos + done;
        write_inode(ip);
    }
    return done || n == 0 ? (int32_t)done : -1;
}

/* 收回文件的所有数据块和间接块，长度变成 0。 */
static void free_indirect(uint32_t zone, int depth)
{
    if (zone == 0)
        return;
    if (depth > 0) {
        struct buffer *b = bread(zone);
        if (b) {
            uint16_t slots[ZONES_PER_BLOCK];
            const uint16_t *src = (const uint16_t *)b->data;
            for (uint32_t i = 0; i < ZONES_PER_BLOCK; i++)
                slots[i] = src[i];
            brelse(b);
            for (uint32_t i = 0; i < ZONES_PER_BLOCK; i++)
                free_indirect(slots[i], depth - 1);
        }
    }
    free_block(zone);
}

static void truncate_inode(struct m_inode *ip)
{
    if ((ip->d.mode & S_IFMT) == S_IFCHR)       /* 设备文件的第一个块号是设备号，不是数据块 */
        return;
    for (int i = 0; i < 7; i++)
        free_indirect(ip->d.zone[i], 0);
    free_indirect(ip->d.zone[7], 1);
    free_indirect(ip->d.zone[8], 2);
    for (int i = 0; i < 9; i++)
        ip->d.zone[i] = 0;
    ip->d.size = 0;
    write_inode(ip);
}

/* open 时带了 O_TRUNC：清空一个普通文件。 */
void fs_truncate(struct m_inode *ip)
{
    truncate_inode(ip);
}

/* ------------------------------------------------------------------ 目录项 */

/* 在目录 dir 里找名字为 name（长 len）的项，返回它的 i 节点号，并给出它在目录里的位置。 */
static uint32_t find_entry(struct m_inode *dir, const char *name, int len, uint32_t *where)
{
    struct dir_entry de;

    for (uint32_t pos = 0; pos + sizeof(de) <= dir->d.size; pos += sizeof(de)) {
        if (inode_read(&dir->d, pos, &de, sizeof(de)) != sizeof(de))
            return 0;
        if (de.inode == 0)
            continue;
        int i = 0;
        while (i < len && de.name[i] == name[i])
            i++;
        if (i == len && (len == NAME_LEN || de.name[len] == '\0')) {
            *where = pos;
            return de.inode;
        }
    }
    return 0;
}

/* 在目录 dir 里加一项：先找空项，没有就接在末尾。 */
static int add_entry(struct m_inode *dir, const char *name, int len, uint32_t ino)
{
    struct dir_entry de;
    uint32_t pos = 0;

    for (; pos + sizeof(de) <= dir->d.size; pos += sizeof(de)) {
        if (inode_read(&dir->d, pos, &de, sizeof(de)) != sizeof(de))
            return -1;
        if (de.inode == 0)
            break;
    }
    de = (struct dir_entry){ .inode = (uint16_t)ino };
    for (int i = 0; i < len; i++)
        de.name[i] = name[i];
    return inode_write(dir, pos, &de, sizeof(de)) == sizeof(de) ? 0 : -1;
}

/*
 * 把路径拆成“所在目录”和“最后一段名字”：返回所在目录的 i 节点（iget 过，用完要 iput），
 * 名字和长度放进 name、len。路径不是 / 开头、中间某一段不存在或不是目录、最后一段为空或太长，返回 0。
 */
static struct m_inode *dir_of(const char *path, const char **name, int *len)
{
    int end = 0;
    while (path[end])
        end++;
    while (end > 1 && path[end - 1] == '/')     /* 去掉末尾的 / */
        end--;
    int start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;
    if (start == 0 || end - start == 0 || end - start > NAME_LEN)
        return 0;
    char parent[PATH_MAX];
    for (int i = 0; i < start; i++)
        parent[i] = path[i];
    parent[start] = '\0';

    uint32_t ino;
    struct minix_inode d;
    if (namei(parent, &ino, &d) < 0 || (d.mode & S_IFMT) != S_IFDIR)
        return 0;
    *name = path + start;
    *len = end - start;
    return iget(ino);
}

/* 新建一个文件或目录，返回它的 i 节点（iget 过）。已经存在、目录不存在、磁盘或 i 节点用完了返回 0。 */
static struct m_inode *create(const char *path, uint16_t mode)
{
    const char *name;
    int len;
    uint32_t where;
    struct m_inode *dir = dir_of(path, &name, &len);

    if (!dir)
        return 0;
    if (find_entry(dir, name, len, &where)) {
        iput(dir);
        return 0;
    }
    uint32_t ino = alloc_bit(2, sb.imap_blocks, sb.ninodes);
    struct m_inode *ip = ino ? iget(ino) : 0;
    if (!ip) {
        if (ino)
            free_bit(2, ino);
        iput(dir);
        return 0;
    }
    ip->d = (struct minix_inode){ .mode = mode, .nlinks = 1 };
    write_inode(ip);
    if (add_entry(dir, name, len, ino) < 0) {
        ip->d.nlinks = 0;
        iput(ip);
        iput(dir);
        return 0;
    }
    iput(dir);
    return ip;
}

/* open 带 O_CREAT 时调用：新建一个空的普通文件。 */
struct m_inode *fs_create(const char *path)
{
    return create(path, S_IFREG | 0644);
}

/* mkdir：新建目录，里面先放 . 和 ..；父目录多了一个子目录，链接数加一（子目录的 .. 指向它）。 */
int fs_mkdir(const char *path)
{
    struct m_inode *ip = create(path, S_IFDIR | 0755);
    if (!ip)
        return -1;
    const char *name;
    int len;
    struct m_inode *dir = dir_of(path, &name, &len);
    int ok = dir && add_entry(ip, ".", 1, ip->ino) == 0 && add_entry(ip, "..", 2, dir->ino) == 0;
    if (ok) {
        ip->d.nlinks = 2;
        write_inode(ip);
        dir->d.nlinks++;
        write_inode(dir);
    }
    iput(dir);
    iput(ip);
    return ok ? 0 : -1;
}

/* 目录除了 . 和 .. 是否是空的。 */
static int dir_empty(struct m_inode *dir)
{
    struct dir_entry de;
    for (uint32_t pos = 2 * sizeof(de); pos + sizeof(de) <= dir->d.size; pos += sizeof(de)) {
        if (inode_read(&dir->d, pos, &de, sizeof(de)) != sizeof(de))
            return 0;
        if (de.inode != 0)
            return 0;
    }
    return 1;
}

/*
 * unlink 和 rmdir：从目录里删掉 path 那一项，被删的 i 节点链接数减一。
 * 文件还开着也可以删：目录里看不见了，打开它的程序还能接着读写，最后一个关掉时才真正收回（iput）。
 * want_dir 为 1 是 rmdir：只删空目录，父目录的链接数也减一（它少了一个 ..）。
 */
static int remove_entry(const char *path, int want_dir)
{
    const char *name;
    int len;
    uint32_t where;
    struct m_inode *dir = dir_of(path, &name, &len);

    if (!dir)
        return -1;
    uint32_t ino = (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')
                       ? 0 : find_entry(dir, name, len, &where);
    struct m_inode *ip = ino ? iget(ino) : 0;
    int is_dir = ip && (ip->d.mode & S_IFMT) == S_IFDIR;
    if (!ip || is_dir != want_dir || (is_dir && !dir_empty(ip))) {
        iput(ip);
        iput(dir);
        return -1;
    }
    struct dir_entry empty = { 0 };
    inode_write(dir, where, &empty, sizeof(empty));
    if (is_dir) {
        ip->d.nlinks = 0;                       /* 它的 . 也不算了 */
        dir->d.nlinks--;
        write_inode(dir);
    } else {
        ip->d.nlinks--;
    }
    write_inode(ip);
    iput(ip);
    iput(dir);
    return 0;
}

int fs_unlink(const char *path)
{
    return remove_entry(path, 0);
}

/* chmod：改文件的权限位（低 12 位），类型不变。Linux 0.11 的 sys_chmod() 也是这样，它还要检查是不是文件的主人。 */
int fs_chmod(const char *path, uint32_t mode)
{
    uint32_t ino;
    struct minix_inode d;

    if (namei(path, &ino, &d) < 0)
        return -1;
    struct m_inode *ip = iget(ino);
    if (!ip)
        return -1;
    ip->d.mode = (uint16_t)((ip->d.mode & S_IFMT) | (mode & 07777));
    write_inode(ip);
    iput(ip);
    return 0;
}

/*
 * link：给已经存在的文件再起一个名字 newpath。两个名字是目录里的两项，指向同一个 i 节点，链接数加一：
 * 文件只有一份，从哪个名字打开都一样。删掉一个名字（unlink 的意思就是“去掉一个链接”）只是链接数减一，
 * 减到 0 才真正收回。目录不行，免得目录树里出现环；新名字已经存在、所在的目录不存在、名字太长也不行。
 * Linux 0.11 的 sys_link() 也是这样。
 */
int fs_link(const char *oldpath, const char *newpath)
{
    uint32_t ino, where;
    struct minix_inode d;
    const char *name;
    int len;

    if (namei(oldpath, &ino, &d) < 0 || (d.mode & S_IFMT) == S_IFDIR)
        return -1;
    struct m_inode *dir = dir_of(newpath, &name, &len);
    if (!dir)
        return -1;
    struct m_inode *ip = find_entry(dir, name, len, &where) ? 0 : iget(ino);
    if (!ip || add_entry(dir, name, len, ino) < 0) {
        iput(ip);
        iput(dir);
        return -1;
    }
    ip->d.nlinks++;
    write_inode(ip);
    iput(ip);
    iput(dir);
    return 0;
}

int fs_rmdir(const char *path)
{
    return remove_entry(path, 1);
}

/* ------------------------------------------------------------------ 统计 */

/* 位图从 first_block 起的 nblocks 块里，第 1 到 limit 位中有几个 0。 */
static int count_free(uint32_t first_block, uint32_t nblocks, uint32_t limit, uint32_t *out)
{
    uint32_t n = 0;
    for (uint32_t blk = 0; blk < nblocks; blk++) {
        struct buffer *b = bread(first_block + blk);
        if (!b)
            return -1;
        for (uint32_t i = 0; i < BLOCK_SIZE * 8; i++) {
            uint32_t bit = blk * BLOCK_SIZE * 8 + i;
            if (bit >= 1 && bit <= limit && !(b->data[i / 8] & (1 << (i % 8))))
                n++;
        }
        brelse(b);
    }
    *out = n;
    return 0;
}

int fs_info(struct fsinfo *info)
{
    if (fs_mount() < 0)
        return -1;
    info->blocks = sb.nzones - sb.firstdatazone;
    info->inodes = sb.ninodes;
    if (count_free(2 + sb.imap_blocks, sb.zmap_blocks, info->blocks, &info->free_blocks) < 0 ||
        count_free(2, sb.imap_blocks, info->inodes, &info->free_inodes) < 0)
        return -1;
    info->dirty = buffer_dirty();
    info->buffers = NR_BUFFERS;
    info->written = buffer_writes;
    return 0;
}
