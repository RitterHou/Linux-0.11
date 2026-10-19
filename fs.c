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
/*
 * 一个 i 节点最多有几个名字。链接数在 Minix v1 的 i 节点里只有一个字节，第 256 个名字会让它回绕成 0，
 * 文件就在还有 255 个名字指着的时候被收回了。Linux 0.11 的 sys_link() 不查，后来 Linux 的 minix 驱动定为 250。
 */
#define LINK_MAX 250

/*
 * 挂着的文件系统：每块盘一个超级块。根文件系统（hda）第一次用到时挂上，别的盘用 mount 系统调用挂到某个目录上：
 * 那个目录的 i 节点记在 covered 里（一直 iget 着），查路径走到这个目录时，换成这块盘的根目录接着查。
 * Linux 0.11 的 super_block[NR_SUPER] 也是这样，s_imount 是挂载点的 i 节点，s_isup 是这块盘的根目录。
 */
struct super {
    uint32_t dev;                   /* 0：这一项空着 */
    struct {                        /* 磁盘上的超级块 */
        uint16_t ninodes;
        uint16_t nzones;
        uint16_t imap_blocks;
        uint16_t zmap_blocks;
        uint16_t firstdatazone;
        uint16_t log_zone_size;
        uint32_t max_size;
        uint16_t magic;
    } __attribute__((packed)) d;
    struct m_inode *covered;        /* 挂在哪个目录上；根文件系统是 0 */
    char on[PATH_MAX];              /* 挂载点的路径，给 df 和 mount 显示 */
};
#define NR_SUPER 4
static struct super supers[NR_SUPER];

static int root_state;              /* 0：根文件系统还没挂；1：挂好了；-1：硬盘上不是 Minix 文件系统 */

/* 设备 dev 挂着的话，返回它的超级块。 */
static struct super *get_super(uint32_t dev)
{
    for (int i = 0; i < NR_SUPER; i++)
        if (supers[i].dev == dev && dev != 0)
            return &supers[i];
    return 0;
}

/* 把 dev 的超级块读进 s，检查魔数。不是 Minix v1 文件系统返回 -1。 */
static int read_super(uint32_t dev, struct super *s)
{
    struct buffer *b = bread(dev, 1);
    if (!b)
        return -1;
    uint8_t *dst = (uint8_t *)&s->d;
    for (uint32_t i = 0; i < sizeof(s->d); i++)
        dst[i] = b->data[i];
    brelse(b);
    if (s->d.magic != SUPER_MAGIC || s->d.log_zone_size != 0)  /* 区块比块大的格式不支持 */
        return -1;
    s->dev = dev;
    return 0;
}

/*
 * 第一次用到文件系统时挂上根文件系统（hda），在第 1 行显示。只能在任务里调用（读硬盘要睡）。
 * Linux 0.11 由 init 进程调用 setup 系统调用，再在 mount_root() 里读根文件系统的超级块。
 */
int fs_mount(void)
{
    if (root_state)
        return root_state > 0 ? 0 : -1;
    struct super *s = &supers[0];
    if (read_super(ROOT_DEV, s) < 0) {
        root_state = -1;
        return -1;
    }
    root_state = 1;
    s->on[0] = '/';
    swap_init(s->d.nzones * 2, hd_sectors[0]);  /* 文件系统后面剩下的扇区是交换区（见 swap.c） */

    struct text_line line;
    line_init(&line);
    line_append(&line, "minix fs: ");
    line_append_dec(&line, s->d.ninodes);
    line_append(&line, " inodes, ");
    line_append_dec(&line, s->d.nzones);
    line_append(&line, " blocks");
    vga_write_at(1, 46, line.text, VGA_GRAY);
    return 0;
}

/* 读 dev 上第 ino 个 i 节点。i 节点表紧跟在两张位图后面。 */
int read_inode(uint32_t dev, uint32_t ino, struct minix_inode *out)
{
    struct super *s = get_super(dev);
    if (!s || ino == 0 || ino > s->d.ninodes)
        return -1;
    uint32_t block = 2 + s->d.imap_blocks + s->d.zmap_blocks + (ino - 1) / INODES_PER_BLOCK;
    struct buffer *b = bread(dev, block);
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
static uint32_t indirect(uint32_t dev, uint32_t zone, uint32_t i)
{
    if (zone == 0)
        return 0;
    struct buffer *b = bread(dev, zone);
    if (!b)
        return 0;
    uint32_t value = ((uint16_t *)b->data)[i];
    brelse(b);
    return value;
}

/* 文件的第 n 块在磁盘上是哪一块；0 表示文件在这里没有块（读出来是 0）。Linux 0.11 的 bmap() 也是这样分三段。 */
static uint32_t bmap(uint32_t dev, const struct minix_inode *ip, uint32_t n)
{
    if (n < 7)
        return ip->zone[n];
    n -= 7;
    if (n < ZONES_PER_BLOCK)
        return indirect(dev, ip->zone[7], n);
    n -= ZONES_PER_BLOCK;
    if (n < ZONES_PER_BLOCK * ZONES_PER_BLOCK)
        return indirect(dev, indirect(dev, ip->zone[8], n / ZONES_PER_BLOCK), n % ZONES_PER_BLOCK);
    return 0;
}

/*
 * 从 dev 上的文件 ip 的第 pos 个字节起读最多 n 个字节到 dst，返回读到的字节数（到了文件末尾就少读），出错返回 -1。
 * dst 可以是用户的缓冲区：复制时可能缺页或写时复制，由缺页异常处理，这时拿着缓冲区也没关系。
 */
int32_t inode_read(uint32_t dev, const struct minix_inode *ip, uint32_t pos, void *dst, uint32_t n)
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
        uint32_t zone = bmap(dev, ip, at / BLOCK_SIZE);
        if (zone == 0) {
            for (uint32_t i = 0; i < chunk; i++)
                out[done + i] = 0;
        } else {
            struct buffer *b = bread(dev, zone);
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

/*
 * 当前任务对 i 节点 ip 有没有 mask（MAY_READ、MAY_WRITE、MAY_EXEC 的组合）这几样权限：有效用户号是文件的主人，
 * 看主人那三位；不是主人、有效组号是文件的组，看组那三位；都不是，看其他人那三位。只看一组：主人自己去掉了写权限，
 * 就算其他人能写，主人也不能写。root 什么都可以，只有执行例外：普通文件一个执行位都没有，root 也不能执行
 * （目录的“执行”是在里面查找，root 照样可以）。Linux 0.11 的 permission() 一样，root 执行的例外写在 do_execve() 里。
 */
int permission(const struct minix_inode *ip, int mask)
{
    uint32_t mode = ip->mode;
    if (suser())
        return !(mask & MAY_EXEC) || (mode & S_IFMT) == S_IFDIR || (mode & 0111);
    if (sched_id(ID_EUID) == ip->uid)
        mode >>= 6;
    else if (sched_id(ID_EGID) == ip->gid)
        mode >>= 3;
    return (mode & (uint32_t)mask & 7) == (uint32_t)mask;
}

/* 路径是不是以 / 结尾（根目录 / 自己不算）。 */
static int trailing_slash(const char *path)
{
    int n = 0;
    while (path[n])
        n++;
    return n > 1 && path[n - 1] == '/';
}

/* 在目录 dir 里找名字为 name（长 len）的项，返回它的 i 节点号；找不到返回 0。读目录出错也返回 0，当作没找到。 */
static uint32_t lookup(uint32_t dev, const struct minix_inode *dir, const char *name, int len)
{
    struct dir_entry de;

    if ((dir->mode & S_IFMT) != S_IFDIR)
        return 0;
    for (uint32_t pos = 0; pos + sizeof(de) <= dir->size; pos += sizeof(de)) {
        if (inode_read(dev, dir, pos, &de, sizeof(de)) != sizeof(de))
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

/* dev 上的 i 节点 ino 上面挂着别的盘的话，返回那块盘的超级块。 */
static struct super *mounted_on(uint32_t dev, uint32_t ino)
{
    for (int i = 0; i < NR_SUPER; i++)
        if (supers[i].dev && supers[i].covered && supers[i].covered->dev == dev && supers[i].covered->ino == ino)
            return &supers[i];
    return 0;
}

/*
 * 按路径找到文件：从根目录起，一段一段地在目录里查下去。成功时返回 0，并给出设备号、i 节点号和 i 节点。
 * 只认 / 开头的绝对路径，相对路径由系统调用先接上当前目录。
 * 跨过挂载点：查到的目录上挂着别的盘，就换成那块盘的根目录；在挂上来的盘的根目录里查 ..，先回到挂载点那个目录再查，
 * 于是 /mnt/.. 是 /。Linux 0.11 的 namei 在 find_entry() 里做同样的两件事。
 */
int namei(const char *path, uint32_t *devp, uint32_t *ino, struct minix_inode *ip)
{
    if (fs_mount() < 0 || path[0] != '/')
        return -1;
    uint32_t dev = ROOT_DEV, n = ROOT_INO;
    if (read_inode(dev, n, ip) < 0)
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
        if (len > NAME_LEN || (ip->mode & S_IFMT) != S_IFDIR || !permission(ip, MAY_EXEC))
            return -1;              /* 在目录里查找要有它的执行权限：/tmp/priv 是 0700，别人连 /tmp/priv/f 都 stat 不到 */
        if (len == 2 && p[0] == '.' && p[1] == '.' && n == ROOT_INO && dev != ROOT_DEV) {
            struct m_inode *mp = get_super(dev)->covered;   /* 挂上来的盘的根目录的 ..：回到挂载点 */
            dev = mp->dev;
            n = mp->ino;
            *ip = mp->d;
        }
        n = lookup(dev, ip, p, len);
        if (n == 0 || read_inode(dev, n, ip) < 0)
            return -1;
        struct super *s = mounted_on(dev, n);
        if (s) {                    /* 这个目录上挂着别的盘：换成那块盘的根目录 */
            dev = s->dev;
            n = ROOT_INO;
            if (read_inode(dev, n, ip) < 0)
                return -1;
        }
        p += len;
    }
    if (trailing_slash(path) && (ip->mode & S_IFMT) != S_IFDIR)
        return -1;                  /* /hello.txt/：末尾的 / 说的是目录，可它是文件 */
    *devp = dev;
    *ino = n;
    return 0;
}

/* ------------------------------------------------------------------ 文件系统锁 */

/*
 * 拿着锁的任务可以再拿一次（只记一下深度）：它在文件系统里往用户的缓冲区复制时，缓冲区可能落在程序还没装入的页上，
 * 缺页处理要从程序文件里读那一页，又要拿这把锁。这一层只读程序文件，而程序文件正在运行时谁也写不了它（见 file.c），
 * 外面那一层正在改的东西不会被它碰到。Linux 0.11 每个 i 节点、每个缓冲区各有一把锁，没有这个问题。
 */
static int fs_busy, fs_owner, fs_depth;
static struct wait_queue fs_wait;

void fs_lock(void)
{
    uint32_t flags = irq_save();
    if (fs_busy && fs_owner == current_pid()) {
        fs_depth++;
        irq_restore(flags);
        return;
    }
    while (fs_busy)
        sleep_on(&fs_wait);
    fs_busy = 1;
    fs_owner = current_pid();
    fs_depth = 1;
    irq_restore(flags);
}

void fs_unlock(void)
{
    uint32_t flags = irq_save();
    if (--fs_depth == 0) {
        fs_busy = 0;
        wake_up(&fs_wait);
    }
    irq_restore(flags);
}

/* ------------------------------------------------------------------ 内存里的 i 节点表 */

#define NR_INODE 32
static struct m_inode inode_table[NR_INODE];

/* 把内存里的 i 节点写回它那块盘上的 i 节点表。 */
static int write_inode(struct m_inode *ip)
{
    struct super *s = get_super(ip->dev);
    if (!s)
        return -1;
    uint32_t block = 2 + s->d.imap_blocks + s->d.zmap_blocks + (ip->ino - 1) / INODES_PER_BLOCK;
    struct buffer *b = bread(ip->dev, block);
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

/* 取 dev 上第 ino 个 i 节点：表里已经有就共用，引用次数加一；没有就从磁盘读进一个空项。失败返回 0。 */
struct m_inode *iget(uint32_t dev, uint32_t ino)
{
    struct m_inode *empty = 0;

    for (int i = 0; i < NR_INODE; i++) {
        if (inode_table[i].count > 0 && inode_table[i].ino == ino && inode_table[i].dev == dev) {
            inode_table[i].count++;
            return &inode_table[i];
        }
        if (inode_table[i].count == 0 && !empty)
            empty = &inode_table[i];
    }
    if (!empty || read_inode(dev, ino, &empty->d) < 0)
        return 0;
    empty->dev = dev;
    empty->ino = ino;
    empty->count = 1;
    empty->writers = empty->execs = 0;
    empty->pipe = 0;
    return empty;
}

static void truncate_inode(struct m_inode *ip);
static void free_bit(uint32_t dev, uint32_t first_block, uint32_t bit);

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
        free_bit(ip->dev, 2, ip->ino);          /* i 节点位图从块 2 开始，第 ino 位就是第 ino 个 i 节点 */
    }
}

/* ------------------------------------------------------------------ 位图 */

/* 在从 first_block 起的 nblocks 块位图里找一个为 0 的位（第 1 到 limit 位），改成 1，返回它；没有返回 0。 */
static uint32_t alloc_bit(uint32_t dev, uint32_t first_block, uint32_t nblocks, uint32_t limit)
{
    for (uint32_t blk = 0; blk < nblocks; blk++) {
        struct buffer *b = bread(dev, first_block + blk);
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

static void free_bit(uint32_t dev, uint32_t first_block, uint32_t bit)
{
    struct buffer *b = bread(dev, first_block + bit / (BLOCK_SIZE * 8));
    if (!b)
        return;
    uint32_t i = bit % (BLOCK_SIZE * 8);
    b->data[i / 8] &= (uint8_t)~(1 << (i % 8));
    bdirty(b);
    brelse(b);
}

/* 在 dev 上分配一个数据块，清零后返回它的块号；满了返回 0。区块位图的第 k 位对应第 firstdatazone + k - 1 块。 */
static uint32_t alloc_block(uint32_t dev)
{
    struct super *s = get_super(dev);
    uint32_t bit = s ? alloc_bit(dev, 2 + s->d.imap_blocks, s->d.zmap_blocks, s->d.nzones - s->d.firstdatazone) : 0;
    if (bit == 0)
        return 0;
    uint32_t block = s->d.firstdatazone + bit - 1;
    struct buffer *b = bget(dev, block);
    if (!b) {                       /* 缓冲区换不出来（脏块写不回去）：把位还回去，不然这一块就再也分不出去了 */
        free_bit(dev, 2 + s->d.imap_blocks, bit);
        return 0;
    }
    /*
     * 清零了的新块：以后写回去，免得文件里读到这一块原来的内容。写回没有先后顺序，突然断电时可能 i 节点和
     * 间接块已经写到硬盘上，这一块还没有，文件里就露出了旧数据；Linux 0.11 的 new_block() 也是这样。
     */
    bdirty(b);
    brelse(b);
    return block;
}

static void free_block(uint32_t dev, uint32_t block)
{
    struct super *s = get_super(dev);
    if (s && block >= s->d.firstdatazone && block < s->d.nzones)
        free_bit(dev, 2 + s->d.imap_blocks, block - s->d.firstdatazone + 1);
}

/* ------------------------------------------------------------------ 写文件 */

/* 间接块 zone 里的第 i 项；create 时没有就分配一块填进去。 */
static uint32_t indirect_slot(uint32_t dev, uint32_t zone, uint32_t i, int create)
{
    struct buffer *b = bread(dev, zone);
    if (!b)
        return 0;
    uint16_t *slots = (uint16_t *)b->data;
    uint32_t value = slots[i];
    if (value == 0 && create) {
        value = alloc_block(dev);
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
        ip->d.zone[i] = (uint16_t)alloc_block(ip->dev);
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
        return ind ? indirect_slot(ip->dev, ind, n, 1) : 0;
    }
    n -= ZONES_PER_BLOCK;
    if (n < ZONES_PER_BLOCK * ZONES_PER_BLOCK) {
        uint32_t dind = zone_slot(ip, 8, 1);
        uint32_t ind = dind ? indirect_slot(ip->dev, dind, n / ZONES_PER_BLOCK, 1) : 0;
        return ind ? indirect_slot(ip->dev, ind, n % ZONES_PER_BLOCK, 1) : 0;
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
        struct buffer *b = bread(ip->dev, zone);
        if (!b)
            break;
        for (uint32_t i = 0; i < chunk; i++)
            b->data[off + i] = in[done + i];
        bdirty(b);
        brelse(b);
        done += chunk;
    }
    if (done) {                     /* 写进了东西：记下修改时间（目录里增删一项也走这里） */
        if (pos + done > ip->d.size)
            ip->d.size = pos + done;
        ip->d.time = current_time();
        write_inode(ip);
    }
    return done || n == 0 ? (int32_t)done : -1;
}

/* 收回文件的所有数据块和间接块，长度变成 0。 */
static void free_indirect(uint32_t dev, uint32_t zone, int depth)
{
    if (zone == 0)
        return;
    if (depth > 0) {
        struct buffer *b = bread(dev, zone);
        if (b) {
            uint16_t slots[ZONES_PER_BLOCK];
            const uint16_t *src = (const uint16_t *)b->data;
            for (uint32_t i = 0; i < ZONES_PER_BLOCK; i++)
                slots[i] = src[i];
            brelse(b);
            for (uint32_t i = 0; i < ZONES_PER_BLOCK; i++)
                free_indirect(dev, slots[i], depth - 1);
        }
    }
    free_block(dev, zone);
}

static void truncate_inode(struct m_inode *ip)
{
    uint32_t type = ip->d.mode & S_IFMT;
    if (type == S_IFCHR || type == S_IFBLK || type == S_IFIFO)  /* 设备文件的第一个块号是设备号，不是数据块；管道没有块 */
        return;
    for (int i = 0; i < 7; i++)
        free_indirect(ip->dev, ip->d.zone[i], 0);
    free_indirect(ip->dev, ip->d.zone[7], 1);
    free_indirect(ip->dev, ip->d.zone[8], 2);
    for (int i = 0; i < 9; i++)
        ip->d.zone[i] = 0;
    ip->d.size = 0;
    ip->d.time = current_time();
    write_inode(ip);
}

/* open 时带了 O_TRUNC：清空一个普通文件。 */
void fs_truncate(struct m_inode *ip)
{
    truncate_inode(ip);
}

/* ------------------------------------------------------------------ 目录项 */

/*
 * 在目录 dir 里找名字为 name（长 len）的项，返回它的 i 节点号，并给出它在目录里的位置；找不到返回 0。
 * 读目录出错返回 ENTRY_ERROR：要是也当成“没找到”，新建文件时就可能在目录里加出两个同名的项。
 */
#define ENTRY_ERROR 0xFFFFFFFFu
static uint32_t find_entry(struct m_inode *dir, const char *name, int len, uint32_t *where)
{
    struct dir_entry de;

    for (uint32_t pos = 0; pos + sizeof(de) <= dir->d.size; pos += sizeof(de)) {
        if (inode_read(dir->dev, &dir->d, pos, &de, sizeof(de)) != sizeof(de))
            return ENTRY_ERROR;
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
        if (inode_read(dir->dev, &dir->d, pos, &de, sizeof(de)) != sizeof(de))
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

    uint32_t dev, ino;
    struct minix_inode d;
    if (namei(parent, &dev, &ino, &d) < 0 || (d.mode & S_IFMT) != S_IFDIR)
        return 0;
    *name = path + start;
    *len = end - start;
    return iget(dev, ino);
}

/*
 * 新建一个文件或目录，返回它的 i 节点（iget 过）。已经存在、目录不存在、磁盘或 i 节点用完了返回 0。
 * 以 / 结尾的路径只能新建目录：open("/tmp/a/", O_CREAT) 不会建出一个叫 a 的普通文件。
 * 要有所在目录的写和执行权限；新文件的主人和组是当前任务的有效用户号和有效组号，与 Linux 0.11 相同。
 */
static struct m_inode *create(const char *path, uint16_t mode)
{
    const char *name;
    int len;
    uint32_t where;
    struct m_inode *dir = (mode & S_IFMT) == S_IFDIR || !trailing_slash(path) ? dir_of(path, &name, &len) : 0;

    if (!dir)
        return 0;
    if (!permission(&dir->d, MAY_WRITE | MAY_EXEC) || find_entry(dir, name, len, &where)) {
        iput(dir);
        return 0;
    }
    struct super *s = get_super(dir->dev);       /* 新文件和它所在的目录在同一块盘上 */
    uint32_t ino = alloc_bit(dir->dev, 2, s->d.imap_blocks, s->d.ninodes);
    struct m_inode *ip = ino ? iget(dir->dev, ino) : 0;
    if (!ip) {
        if (ino)
            free_bit(dir->dev, 2, ino);
        iput(dir);
        return 0;
    }
    ip->d = (struct minix_inode){ .mode = mode, .nlinks = 1, .time = current_time(),
                                  .uid = (uint16_t)sched_id(ID_EUID), .gid = (uint8_t)sched_id(ID_EGID) };
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

/*
 * mkdir：新建目录，里面先放 . 和 ..；父目录多了一个子目录，链接数加一（子目录的 .. 指向它）。
 * 父目录的链接数已经到了 LINK_MAX（248 个子目录），就不能再建。
 */
int fs_mkdir(const char *path)
{
    const char *name;
    int len;
    struct m_inode *dir = dir_of(path, &name, &len);
    if (!dir)
        return -1;
    struct m_inode *ip = dir->d.nlinks < LINK_MAX ? create(path, S_IFDIR | 0755) : 0;
    if (!ip) {
        iput(dir);
        return -1;
    }
    int ok = add_entry(ip, ".", 1, ip->ino) == 0 && add_entry(ip, "..", 2, dir->ino) == 0;
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
        if (inode_read(dir->dev, &dir->d, pos, &de, sizeof(de)) != sizeof(de))
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
 * 要有所在目录的写和执行权限，不用有文件本身的：删的是目录里的一项。目录有粘滞位（/tmp）时，还得是文件的主人、
 * 目录的主人或 root，不然谁都能删掉别人放在 /tmp 里的文件。Linux 0.11 没有粘滞位。
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
    struct m_inode *ip = ino && ino != ENTRY_ERROR && permission(&dir->d, MAY_WRITE | MAY_EXEC) ? iget(dir->dev, ino) : 0;
    int is_dir = ip && (ip->d.mode & S_IFMT) == S_IFDIR;
    uint32_t euid = sched_id(ID_EUID);
    if (ip && (((dir->d.mode & S_ISVTX) && !suser() && euid != ip->d.uid && euid != dir->d.uid) || mounted_on(ip->dev, ip->ino))) {
        iput(ip);                   /* 粘滞位不让删；或者上面挂着别的盘（rmdir /mnt）：先 umount */
        ip = 0;
    }
    /* unlink("/tmp/a/")：末尾的 / 说的是目录，a 却是文件，也不删 */
    if (!ip || is_dir != want_dir || (!is_dir && trailing_slash(path)) || (is_dir && !dir_empty(ip))) {
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

/*
 * utime：把文件的修改时间设成 t，t 为 0 就是现在。Linux 0.11 的 sys_utime() 同时设访问时间，Minix v1 的 i 节点只有一个时间。
 * 设成现在，有写权限就行（touch 别人允许你写的文件）；设成任意时间，得是主人或 root，与 POSIX 相同。Linux 0.11 不查。
 */
int fs_utime(const char *path, uint32_t t)
{
    uint32_t dev, ino;
    struct minix_inode d;

    if (namei(path, &dev, &ino, &d) < 0)
        return -1;
    if (!suser() && sched_id(ID_EUID) != d.uid && (t || !permission(&d, MAY_WRITE)))
        return -1;
    struct m_inode *ip = iget(dev, ino);
    if (!ip)
        return -1;
    ip->d.time = t ? t : current_time();
    write_inode(ip);
    iput(ip);
    return 0;
}

/*
 * chmod：改文件的权限位（低 12 位），类型不变。只有文件的主人和 root 能改，与 Linux 0.11 的 sys_chmod() 相同。
 * 不是 root、自己又不在文件的组里，setgid 位设不上（POSIX 的规定）：不然能做出一个让别人以那个组运行的程序。
 */
int fs_chmod(const char *path, uint32_t mode)
{
    uint32_t dev, ino;
    struct minix_inode d;

    if (namei(path, &dev, &ino, &d) < 0 || (!suser() && sched_id(ID_EUID) != d.uid))
        return -1;
    if (!suser() && sched_id(ID_EGID) != d.gid)
        mode &= ~(uint32_t)S_ISGID;
    struct m_inode *ip = iget(dev, ino);
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
 * 减到 0 才真正收回。目录不行，免得目录树里出现环；新名字已经存在、所在的目录不存在、名字太长、以 / 结尾
 * （那说的是目录）也不行。两个名字还得在同一块盘上：目录项里只有 i 节点号，指不到别的盘上去。
 * Linux 0.11 的 sys_link() 也是这样，只是没有查链接数到没到 LINK_MAX。
 */
int fs_link(const char *oldpath, const char *newpath)
{
    uint32_t dev, ino, where;
    struct minix_inode d;
    const char *name;
    int len;

    if (namei(oldpath, &dev, &ino, &d) < 0 || (d.mode & S_IFMT) == S_IFDIR)
        return -1;
    struct m_inode *dir = trailing_slash(newpath) ? 0 : dir_of(newpath, &name, &len);
    if (!dir)
        return -1;
    if (!permission(&dir->d, MAY_WRITE | MAY_EXEC) || dir->dev != dev) {    /* 新名字所在的目录要能写，在同一块盘上 */
        iput(dir);
        return -1;
    }
    struct m_inode *ip = find_entry(dir, name, len, &where) ? 0 : iget(dev, ino);
    if (!ip || ip->d.nlinks >= LINK_MAX || add_entry(dir, name, len, ino) < 0) {
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

/*
 * chown：改文件的主人和组，-1（0xFFFFFFFF）表示这一样不改。只有 root 能改，与 Linux 0.11 的 sys_chown() 相同：
 * 要是谁都能把文件送给别人，配额和 setuid 程序都没法管。普通文件改了主人，setuid、setgid 位都清掉，
 * 免得一个 setuid 程序换了主人以后，以新主人的身份运行。Minix v1 的组号只有一个字节，大于 255 不行。
 */
int fs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    uint32_t dev, ino;
    struct minix_inode d;

    if (namei(path, &dev, &ino, &d) < 0 || !suser() || (uid != 0xFFFFFFFF && uid > 0xFFFF) || (gid != 0xFFFFFFFF && gid > 0xFF))
        return -1;
    struct m_inode *ip = iget(dev, ino);
    if (!ip)
        return -1;
    if (uid != 0xFFFFFFFF)
        ip->d.uid = (uint16_t)uid;
    if (gid != 0xFFFFFFFF)
        ip->d.gid = (uint8_t)gid;
    if ((ip->d.mode & S_IFMT) == S_IFREG)
        ip->d.mode &= (uint16_t)~(S_ISUID | S_ISGID);
    write_inode(ip);
    iput(ip);
    return 0;
}

/*
 * mknod：新建一个特殊文件。命名管道（S_IFIFO）谁都能建；字符设备、块设备只有 root 能建，dev 记在第一个区块号里：
 * 谁都能建设备文件的话，建一个指向硬盘的、自己能读写的设备文件，权限就形同虚设了。
 * Linux 0.11 的 sys_mknod() 只有 root 能调用，它的管道只有 pipe 建的那种，没有命名管道。
 */
int fs_mknod(const char *path, uint32_t mode, uint32_t dev)
{
    uint32_t type = mode & S_IFMT;
    if ((type != S_IFIFO && type != S_IFCHR && type != S_IFBLK) || (type != S_IFIFO && !suser()))
        return -1;
    struct m_inode *ip = create(path, (uint16_t)(type | (mode & 07777)));
    if (!ip)
        return -1;
    if (type != S_IFIFO) {
        ip->d.zone[0] = (uint16_t)dev;
        write_inode(ip);
    }
    iput(ip);
    return 0;
}

int fs_rmdir(const char *path)
{
    return remove_entry(path, 1);
}

/* ------------------------------------------------------------------ 统计 */

/* 位图从 first_block 起的 nblocks 块里，第 1 到 limit 位中有几个 0。 */
static int count_free(uint32_t dev, uint32_t first_block, uint32_t nblocks, uint32_t limit, uint32_t *out)
{
    uint32_t n = 0;
    for (uint32_t blk = 0; blk < nblocks; blk++) {
        struct buffer *b = bread(dev, first_block + blk);
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

/* 第 n 个挂着的文件系统（0 是根文件系统）有多少块、多少 i 节点，各还剩多少，挂在哪里。没有第 n 个返回 -1。 */
int fs_info(uint32_t n, struct fsinfo *info)
{
    if (fs_mount() < 0)
        return -1;
    struct super *s = 0;
    for (int i = 0; i < NR_SUPER && !s; i++) {
        if (supers[i].dev && n == 0)
            s = &supers[i];
        else if (supers[i].dev)
            n--;
    }
    if (!s)
        return -1;
    info->dev = s->dev;
    for (int i = 0; i < PATH_MAX; i++)
        info->on[i] = s->on[i];
    info->blocks = s->d.nzones - s->d.firstdatazone;
    info->inodes = s->d.ninodes;
    if (count_free(s->dev, 2 + s->d.imap_blocks, s->d.zmap_blocks, info->blocks, &info->free_blocks) < 0 ||
        count_free(s->dev, 2, s->d.imap_blocks, info->inodes, &info->free_inodes) < 0)
        return -1;
    info->swap_total = swap_total;
    info->swap_used = swap_used;
    info->free_pages = free_page_count();
    info->dirty = buffer_dirty();
    info->buffers = NR_BUFFERS;
    info->written = buffer_writes;
    return 0;
}

/* ------------------------------------------------------------------ 挂载 */

/*
 * mount：把块设备 devpath（例如 /dev/hdb）上的文件系统挂到目录 dirpath 上，此后 dirpath 就是那块盘的根目录，
 * 原来目录里的东西看不见了，umount 以后才又出来。只有 root 能挂；目录不能是根目录（也不能是别的盘的根目录），
 * 不能有人正在用它（打开着），上面也不能已经挂着东西；这块盘也不能已经挂在别处。on 是规范化过的 dirpath，df 和 mount 显示它。
 * Linux 0.11 的 sys_mount() 做同样的检查：i_count 为 1、不是根目录、i_mount 为 0，再 read_super()。
 */
int fs_do_mount(const char *devpath, const char *dirpath, const char *on)
{
    uint32_t dev, ino, target;
    struct minix_inode d;

    if (!suser() || namei(devpath, &dev, &ino, &d) < 0 || (d.mode & S_IFMT) != S_IFBLK)
        return -1;
    target = d.zone[0];
    if (get_super(target) || namei(dirpath, &dev, &ino, &d) < 0 || (d.mode & S_IFMT) != S_IFDIR || ino == ROOT_INO)
        return -1;
    struct super *s = 0;
    for (int i = 0; i < NR_SUPER && !s; i++)
        if (supers[i].dev == 0)
            s = &supers[i];
    struct m_inode *dir = s ? iget(dev, ino) : 0;
    if (!dir || dir->count > 1 || mounted_on(dev, ino) || read_super(target, s) < 0) {
        if (s)
            s->dev = 0;
        iput(dir);
        return -1;
    }
    s->covered = dir;               /* 一直 iget 着，umount 时才放手 */
    for (int i = 0; i < PATH_MAX; i++)
        s->on[i] = on[i];
    return 0;
}

/*
 * umount：卸下一块盘。path 可以是块设备（/dev/hdb），也可以是挂载点（/mnt，查到的就是那块盘的根目录）。
 * 根文件系统卸不了；那块盘上还有 i 节点有人在用（打开着的文件），也卸不了。先把它的脏块写回、缓冲区清空，
 * 再放开挂载点的目录。当前目录记的是路径，不引用 i 节点，所以 cd 在 /mnt 里也能卸，卸了以后那个路径指向原来的目录。
 * Linux 0.11 的 sys_umount() 也检查 i 节点表里还有没有这块盘的、sync_dev()，再放开 s_imount。
 */
int fs_do_umount(const char *path)
{
    uint32_t dev, ino, target = 0;
    struct minix_inode d;

    if (!suser() || namei(path, &dev, &ino, &d) < 0)
        return -1;
    if ((d.mode & S_IFMT) == S_IFBLK)
        target = d.zone[0];
    else if ((d.mode & S_IFMT) == S_IFDIR && ino == ROOT_INO)
        target = dev;
    struct super *s = get_super(target);
    if (!s || target == ROOT_DEV)
        return -1;
    for (int i = 0; i < NR_INODE; i++)
        if (inode_table[i].count > 0 && inode_table[i].dev == target)
            return -1;              /* 还有人在用 */
    if (bflush_dev(target) < 0)
        return -1;
    iput(s->covered);
    s->covered = 0;
    s->dev = 0;
    return 0;
}
