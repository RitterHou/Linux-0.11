/*
 * 缓冲区缓存：最近用过的磁盘块留在内存里，再读同一块就不用访问硬盘。
 * 思路与 Linux 0.11 的 fs/buffer.c 相同：bread(block) 返回一个装着那一块的缓冲区，用完调用 brelse()。
 *   - 缓冲区里已经有这一块，直接给出去（命中）；
 *   - 没有，就挑一个没人在用、最久没用过的缓冲区，换成这一块，从硬盘读进来（未命中）；
 *   - 别的任务正在把这一块读进来（locked），或者 16 个缓冲区全都有人在用，就睡下等。
 *
 * 改了缓冲区里的内容，马上用 bwrite() 写回硬盘（写穿）：断电也不会丢，代价是每次修改都要等硬盘。
 * 新分配的块用 bget() 拿缓冲区：内容全部清零，不必先从硬盘读进来。
 * Linux 0.11 用散列表加空闲链表管理几百个缓冲区，改过的块先标成“脏”，攒到 sync 时再一起写回；
 * 这里只有 16 个，按块号挨个找。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"

#define NR_BUFFERS 16

static struct buffer buffers[NR_BUFFERS];
static struct wait_queue buffer_wait;   /* 等某个缓冲区读完、或者等有缓冲区空出来 */
static uint32_t clock;                  /* 每用一次缓冲区加一，用来比较谁最久没用 */
uint32_t buffer_hits, buffer_misses, buffer_writes;

static struct buffer *find(uint32_t block)
{
    for (int i = 0; i < NR_BUFFERS; i++)
        if (buffers[i].block == block)
            return &buffers[i];
    return 0;
}

static struct buffer *pick_victim(void)
{
    struct buffer *best = 0;
    for (int i = 0; i < NR_BUFFERS; i++) {
        struct buffer *b = &buffers[i];
        if (b->count == 0 && !b->locked && (!best || b->last_used < best->last_used))
            best = b;
    }
    return best;
}

static struct buffer *get_block(uint32_t block, int read);

/* 读第 block 块（1024 字节，即第 2 × block 和 2 × block + 1 扇区）。读不出来返回 0。 */
struct buffer *bread(uint32_t block)
{
    return get_block(block, 1);
}

/* 要整块重写的新块：拿一个缓冲区，内容清零，不从硬盘读。之后要 bwrite() 才会写到硬盘上。 */
struct buffer *bget(uint32_t block)
{
    return get_block(block, 0);
}

/* 把缓冲区的内容写回硬盘。调用者正拿着它（count 不为 0），别人不会把它换成别的块。 */
int bwrite(struct buffer *b)
{
    int ok = hd_write(2 * b->block, b->data) == 0 && hd_write(2 * b->block + 1, b->data + 512) == 0;
    if (ok)
        buffer_writes++;
    return ok ? 0 : -1;
}

static struct buffer *get_block(uint32_t block, int read)
{
    uint32_t flags = irq_save();

    if (clock == 0)                 /* 第一次用：把所有缓冲区标成空的（.bss 里是 0，0 是合法的块号） */
        for (int i = 0; i < NR_BUFFERS; i++)
            buffers[i].block = NO_BLOCK;
    for (;;) {
        struct buffer *b = find(block);
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
        b->block = block;
        b->valid = 0;
        b->locked = 1;
        b->count = 1;
        irq_restore(flags);

        int ok = 1;
        if (read) {
            buffer_misses++;
            ok = hd_read(2 * block, b->data) == 0 && hd_read(2 * block + 1, b->data + 512) == 0;
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
