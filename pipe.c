/*
 * 管道：一页大小的环形缓冲区，一头写、一头读，与 Linux 0.11 的 fs/pipe.c 对应。
 *   - 读：管道空着、还有人拿着写的一头，就睡下等；有数据就读走（最多读到要的字节数），不等凑满；
 *     管道空了、写的一头全都关了，返回 0，读的一方由此知道数据到头了。
 *   - 写：管道满了、还有人拿着读的一头，就睡下等；读的一头全都关了，再写也没人看，返回 -1
 *     （Linux 会给写的进程发 SIGPIPE，这里还没有信号）。
 *   - 两头都关了，还掉缓冲区那一页。
 * 读写双方睡在同一个等待队列上，每次读走或写进数据都叫醒对方。Linux 0.11 的管道借用一个没有文件名的
 * i 节点，缓冲区就是一页，头尾指针放在 i_zone[0]、i_zone[1] 里，原理相同。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"
#include "syscall.h"

#define NR_PIPES 8
#define CHUNK    512                /* 每次在关中断时搬多少字节；和用户内存之间的复制在开中断时做 */

static struct pipe pipes[NR_PIPES];

/* 建一个管道，返回它；没有空位或没有内存时返回 0。读写两头的计数由调用者在打开文件时加上。 */
struct pipe *pipe_new(void)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < NR_PIPES; i++) {
        struct pipe *p = &pipes[i];
        if (p->buf == 0 && p->readers == 0 && p->writers == 0) {
            uint32_t page = get_free_page();
            if (page == 0)
                break;
            *p = (struct pipe){ .buf = (uint8_t *)page };
            irq_restore(flags);
            return p;
        }
    }
    irq_restore(flags);
    return 0;
}

/* 关掉一头。两头都关了，还掉缓冲区。 */
void pipe_close(struct pipe *p, int writer)
{
    uint32_t flags = irq_save();
    if (writer)
        p->writers--;
    else
        p->readers--;
    wake_up(&p->wait);              /* 让另一头知道：可能读到末尾了，或者写了也没人读 */
    if (p->readers == 0 && p->writers == 0) {
        free_page((uint32_t)p->buf);
        p->buf = 0;
    }
    irq_restore(flags);
}

int32_t pipe_read(struct pipe *p, void *dst, uint32_t n)
{
    uint8_t tmp[CHUNK];
    uint32_t got = 0;
    uint32_t flags = irq_save();

    while (p->head == p->tail && p->writers > 0) {
        if (interruptible_sleep_on(&p->wait) < 0) {
            irq_restore(flags);
            return -EINTR;
        }
    }
    while (got < n && got < CHUNK && p->tail != p->head)
        tmp[got++] = p->buf[p->tail++ % PAGE_SIZE];
    wake_up(&p->wait);              /* 腾出了地方：叫醒等着写的 */
    irq_restore(flags);
    for (uint32_t i = 0; i < got; i++)
        ((uint8_t *)dst)[i] = tmp[i];
    return (int32_t)got;
}

int32_t pipe_write(struct pipe *p, const void *src, uint32_t n)
{
    uint8_t tmp[CHUNK];
    uint32_t done = 0;

    while (done < n) {
        uint32_t chunk = n - done < CHUNK ? n - done : CHUNK;
        for (uint32_t i = 0; i < chunk; i++)        /* 先从用户内存复制出来：可能缺页 */
            tmp[i] = ((const uint8_t *)src)[done + i];
        uint32_t flags = irq_save();
        for (uint32_t i = 0; i < chunk;) {
            while (p->head - p->tail == PAGE_SIZE && p->readers > 0) {
                if (interruptible_sleep_on(&p->wait) < 0) {
                    irq_restore(flags);
                    return done + i ? (int32_t)(done + i) : -EINTR;     /* 写进去的不能收回，就报告写了多少 */
                }
            }
            if (p->readers == 0) {
                /* 没人读了：写的进程收到 SIGPIPE，默认就被结束。Linux 0.11 的 write_pipe() 也是这样 */
                signal_current(SIGPIPE);
                irq_restore(flags);
                return done + i ? (int32_t)(done + i) : -1;
            }
            while (i < chunk && p->head - p->tail < PAGE_SIZE)
                p->buf[p->head++ % PAGE_SIZE] = tmp[i++];
            wake_up(&p->wait);      /* 有数据了：叫醒等着读的 */
        }
        irq_restore(flags);
        done += chunk;
    }
    return (int32_t)done;
}
