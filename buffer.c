/*
 * 缓冲区缓存：最近用过的磁盘块留在内存里，再读同一块就不用访问硬盘。
 * 思路与 Linux 0.11 的 fs/buffer.c 相同：bread(dev, block) 返回一个装着 dev 那块盘第 block 块的缓冲区，用完调用 brelse()。
 * 一个缓冲区由设备号和块号认出来：两块盘上的第 5 块是不同的两块。
 *   - 缓冲区里已经有这一块，直接给出去（命中）；
 *   - 没有，就挑一个没人在用、最久没用过的缓冲区，换成这一块，从硬盘读进来（未命中）；
 *   - 别的任务正在把这一块读进来（locked），或者 16 个缓冲区全都有人在用，就睡下等。
 *
 * 改了缓冲区里的内容，调用 bdirty() 把它标成“脏”的，并不马上写盘（延迟写回）：同一块改好几次，
 * 最后只写一次。脏块在两种时候写回硬盘：
 *   - 缓冲区要换成别的块，挑中的又是脏的（先挑干净的，全是脏的才挑脏的）：先写回再换；
 *   - sync()：把所有脏块写回。init 每隔大约 5 秒调用一次 sync 系统调用，所以突然关机丢掉的是最近几秒的修改。
 *     写回没有先后顺序，断电时可能一部分块写到了、一部分没有（比如目录项写到了，它指的 i 节点还没有），
 *     文件系统就不一致了，要靠 fsck 这样的工具检查修补；Linux 0.11 也是这样。
 * 新分配的块用 bget() 拿缓冲区：内容全部清零，不必先从硬盘读进来。
 * 硬盘写出错的情况只做了最基本的处理：换块时脏块写不回去，这一块留着不丢，这次 bread 失败，失败报给了正好要读别的块的
 * 调用者；一块总也写不回去，缓冲区又都是脏的，以后每次未命中都会失败。init 也不看 sync 的返回值。
 * Linux 0.11 也是这样：bh->b_dirt 标脏，getblk() 换块时尽量挑不脏的，sys_sync() 写回所有脏块，
 * 用户态的 update 进程每 30 秒调用一次 sync。它用散列表加空闲链表管理几百个缓冲区；这里只有 16 个，按块号挨个找。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"

static struct buffer buffers[NR_BUFFERS];
static struct wait_queue buffer_wait;   /* 等某个缓冲区读完、或者等有缓冲区空出来 */
static uint32_t clock;                  /* 每用一次缓冲区加一，用来比较谁最久没用 */
uint32_t buffer_hits, buffer_misses, buffer_writes;    /* buffer_writes：写回硬盘的块数 */

static struct buffer *find(uint32_t dev, uint32_t block)
{
    for (int i = 0; i < NR_BUFFERS; i++)
        if (buffers[i].block == block && buffers[i].dev == dev)
            return &buffers[i];
    return 0;
}

/* 设备号对应哪块盘：hda 是主盘 0，hdb 是从盘 1；别的设备不是硬盘，返回 -1。 */
static int drive_of(uint32_t dev)
{
    return dev == ROOT_DEV ? 0 : dev == HDB_DEV ? 1 : -1;
}

/* 挑一个没人在用的缓冲区换成别的块：干净的里面最久没用的；没有干净的，才挑脏的（要先写回）。 */
static struct buffer *pick_victim(void)
{
    struct buffer *best = 0;
    for (int i = 0; i < NR_BUFFERS; i++) {
        struct buffer *b = &buffers[i];
        if (b->count != 0 || b->locked)
            continue;
        if (!best || (best->dirty && !b->dirty) || (best->dirty == b->dirty && b->last_used < best->last_used))
            best = b;
    }
    return best;
}

/* 把缓冲区写回硬盘（两个扇区）。调用者已经把它锁上（locked），别人要用这一块得等着。成功返回 0。 */
static int write_back(struct buffer *b)
{
    int drive = drive_of(b->dev);
    int ok = hd_write(drive, 2 * b->block, b->data) == 0 && hd_write(drive, 2 * b->block + 1, b->data + 512) == 0;
    if (!ok)
        return -1;
    b->dirty = 0;
    buffer_writes++;
    return 0;
}

static struct buffer *get_block(uint32_t dev, uint32_t block, int read);

/* 读第 block 块（1024 字节，即第 2 × block 和 2 × block + 1 扇区）。读不出来返回 0。 */
struct buffer *bread(uint32_t dev, uint32_t block)
{
    return get_block(dev, block, 1);
}

/* 要整块重写的新块：拿一个缓冲区，内容清零，不从硬盘读。改完要 bdirty()，以后才会写到硬盘上。 */
struct buffer *bget(uint32_t dev, uint32_t block)
{
    return get_block(dev, block, 0);
}

/* 缓冲区的内容改过了，以后要写回硬盘。调用者正拿着它（count 不为 0）。 */
void bdirty(struct buffer *b)
{
    b->dirty = 1;
}

/*
 * 把所有脏块写回硬盘，返回 0；有写不进去的返回 -1（它们还是脏的）。调用者拿着文件系统锁：
 * 写的时候不会有人在改缓冲区。正写着的缓冲区锁上，也不能被换走。
 */
int bsync(void)
{
    int ret = 0;
    for (int i = 0; i < NR_BUFFERS; i++) {
        struct buffer *b = &buffers[i];
        uint32_t flags = irq_save();
        if (!b->dirty || b->locked) {
            irq_restore(flags);
            continue;
        }
        b->locked = 1;
        b->count++;
        irq_restore(flags);
        if (write_back(b) < 0)
            ret = -1;
        flags = irq_save();
        b->locked = 0;
        b->count--;
        wake_up(&buffer_wait);
        irq_restore(flags);
    }
    return ret;
}

/*
 * 卸下一块盘时调用：把这块盘的脏块写回去，再把它的缓冲区都清空，免得以后换了一块盘还读到旧的内容。
 * 有缓冲区还有人在用，或者写不回去，返回 -1。调用者拿着文件系统锁。Linux 0.11 的 sys_umount() 调用 sync_dev()。
 */
int bflush_dev(uint32_t dev)
{
    if (bsync() < 0)
        return -1;
    uint32_t flags = irq_save();
    for (int i = 0; i < NR_BUFFERS; i++) {
        struct buffer *b = &buffers[i];
        if (b->dev != dev || b->block == NO_BLOCK)
            continue;
        if (b->count || b->locked || b->dirty) {
            irq_restore(flags);
            return -1;
        }
        b->block = NO_BLOCK;
        b->valid = 0;
    }
    irq_restore(flags);
    return 0;
}

/* 现在有几个脏的缓冲区：df 显示。 */
uint32_t buffer_dirty(void)
{
    uint32_t n = 0;
    for (int i = 0; i < NR_BUFFERS; i++)
        n += buffers[i].dirty;
    return n;
}

static struct buffer *get_block(uint32_t dev, uint32_t block, int read)
{
    uint32_t flags = irq_save();

    if (clock == 0)                 /* 第一次用：把所有缓冲区标成空的（.bss 里是 0，0 是合法的块号） */
        for (int i = 0; i < NR_BUFFERS; i++)
            buffers[i].block = NO_BLOCK;
    for (;;) {
        struct buffer *b = find(dev, block);
        if (b && b->locked) {       /* 别人正在读这一块：等它读完，再从头找 */
            sleep_on(&buffer_wait);
            continue;
        }
        if (b && b->valid) {
            b->count++;
            b->last_used = ++clock;
            buffer_hits++;
            irq_restore(flags);
            if (!read)                      /* 新块：不管缓冲区里原来是什么，都从全 0 开始 */
                for (int i = 0; i < BLOCK_SIZE; i++)
                    b->data[i] = 0;
            return b;
        }
        if (!b)
            b = pick_victim();
        if (!b) {                   /* 16 个缓冲区都有人在用 */
            sleep_on(&buffer_wait);
            continue;
        }
        b->locked = 1;
        b->count = 1;
        if (b->dirty) {             /* 挑中的是脏的：先把它原来那一块写回硬盘，再换 */
            irq_restore(flags);
            int written = write_back(b) == 0;
            flags = irq_save();
            if (!written) {         /* 写不进去：这一块不能丢，不换了，这次读算失败 */
                b->locked = 0;
                b->count = 0;
                wake_up(&buffer_wait);
                irq_restore(flags);
                return 0;
            }
            if (find(dev, block)) { /* 写回的时候睡过：别人可能已经把这一块读进了别的缓冲区，那就用那一个 */
                b->locked = 0;
                b->count = 0;
                wake_up(&buffer_wait);
                continue;
            }
        }
        b->dev = dev;
        b->block = block;
        b->valid = 0;
        irq_restore(flags);

        int ok = 1;
        if (read) {
            buffer_misses++;
            int drive = drive_of(dev);
            ok = hd_read(drive, 2 * block, b->data) == 0 && hd_read(drive, 2 * block + 1, b->data + 512) == 0;
        } else {
            for (int i = 0; i < BLOCK_SIZE; i++)
                b->data[i] = 0;
        }

        flags = irq_save();
        b->locked = 0;
        b->valid = ok;
        b->last_used = ++clock;
        wake_up(&buffer_wait);
        if (!ok) {
            b->count = 0;
            b->block = NO_BLOCK;
            b = 0;
        }
        irq_restore(flags);
        return b;
    }
}

void brelse(struct buffer *b)
{
    uint32_t flags = irq_save();
    if (b->count == 0)
        panic("trying to free free buffer");
    b->count--;
    wake_up(&buffer_wait);
    irq_restore(flags);
}
