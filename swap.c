/*
 * 交换：物理页快用完时，把用户的页写到硬盘上的交换区，腾出物理页；程序再用到它时，缺页处理把它读回来。
 *
 * 交换区是 hda 上根文件系统后面的那一段：mkfs 做的文件系统只占镜像的前 4 MB，Makefile 在后面又接了 4 MB 的空白。
 * 挂根文件系统时（fs_mount）知道文件系统多大，剩下的就是交换区，每一页占 8 个扇区，最多 1024 页。
 * swap_map 记着每个交换页被几个页表项引用：fork 时父子的页表项指向同一个交换页，谁先读回来谁减一，减到 0 才空出来。
 *
 * 页表项：存在位是 0、整个又不是 0 的，就是换出去了，高 20 位是交换页的号加一。存在位为 0，CPU 不看别的位。
 *
 * 换哪一页出去（swap_out）：时钟算法，也叫第二次机会。一根指针在所有任务的用户页上转圈：
 *   - 页表项的访问位（CPU 每次用到这一页就置上）是 1 的，清掉它，跳过：最近用过，再给一次机会；
 *   - 好几个页表项共用的页（fork 后写时复制的、和别的进程共用的程序页）不换：一个一个改太麻烦；
 *   - 没写过的页（脏位是 0）不用写到硬盘上，直接扔掉：程序的页再从程序文件读，堆和栈的页本来就全是 0；
 *   - 写过的页：分一个交换页，页表项先改成“换出去了”，再写到硬盘上，最后放开物理页。
 * 写硬盘要睡，所以换进换出拿着一把锁（swap_lock）：正在写的页，它的主人要是正好用到、来读，就等着写完再读。
 *
 * 谁来调用：缺页处理和 fork、exec 这些要分物理页、又能睡的地方，看空闲页少于 SWAP_LOW 就先换出去几页（mem_reclaim）。
 * 留着 SWAP_LOW 页，给关着中断、不能睡的地方（页表、内核栈、管道）用。
 * Linux 0.11 没有交换，0.12 加了 mm/swap.c：get_free_page 分不到页就调用 swap_out，也是转着圈找一页写到交换区。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"

#define SWAP_MAX     1024           /* 交换区最多这么多页 */
#define PTE_PRESENT  0x001
#define PTE_WRITE    0x002
#define PTE_ACCESSED 0x020          /* CPU 用到这一页时置上 */
#define PTE_DIRTY    0x040          /* CPU 写这一页时置上 */

static uint8_t swap_map[SWAP_MAX];  /* 每个交换页被几个页表项引用；0 是空着 */
static uint32_t swap_start;         /* 交换区从 hda 的第几个扇区开始 */
uint32_t swap_total, swap_used;     /* 交换区一共几页、用了几页（df 显示） */

static int swap_busy;
static struct wait_queue swap_wait;
static int clock_task = 1;          /* 时钟的指针：转到第几个任务的第几页了 */
static uint32_t clock_index;

/* 挂上根文件系统时调用：hda 上从 start 扇区到 end 扇区是交换区 */
void swap_init(uint32_t start, uint32_t end)
{
    swap_start = start;
    swap_total = end > start ? (end - start) / 8 : 0;
    if (swap_total > SWAP_MAX)
        swap_total = SWAP_MAX;
}

static void swap_lock(void)
{
    uint32_t flags = irq_save();
    while (swap_busy)
        sleep_on(&swap_wait);
    swap_busy = 1;
    irq_restore(flags);
}

static void swap_unlock(void)
{
    uint32_t flags = irq_save();
    swap_busy = 0;
    wake_up(&swap_wait);
    irq_restore(flags);
}

/* 读写交换区的第 slot 页：8 个扇区。物理页在一一对应映射的范围里，按物理地址直接读写。 */
static int swap_io(uint32_t slot, uint32_t page, int write)
{
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t sector = swap_start + slot * 8 + i;
        void *buf = (void *)(page + i * 512);
        if ((write ? hd_write(0, sector, buf) : hd_read(0, sector, buf)) < 0)
            return -1;
    }
    return 0;
}

/* fork 时子进程的页表项也指向这个交换页 */
void swap_dup(uint32_t entry)
{
    uint32_t flags = irq_save();
    swap_map[(entry >> 12) - 1]++;
    irq_restore(flags);
}

/* 一个页表项不再指向这个交换页了（读回来了，或者任务结束了） */
void swap_free(uint32_t entry)
{
    uint32_t flags = irq_save();
    uint32_t slot = (entry >> 12) - 1;
    if (slot < SWAP_MAX && swap_map[slot] && --swap_map[slot] == 0)
        swap_used--;
    irq_restore(flags);
}

/*
 * 缺页处理调用：页表项 entry 换出去了，把它读回物理页 page，返回 0；读不出来返回 -1。
 * 调用者开着中断（要睡），之后把页表项改成指向 page。
 */
int swap_in(uint32_t entry, uint32_t page)
{
    swap_lock();                    /* 这一页可能正在被写出去：等写完 */
    int ok = swap_io((entry >> 12) - 1, page, 0) == 0;
    swap_unlock();
    if (ok)
        swap_free(entry);
    return ok ? 0 : -1;
}

/*
 * 换出去一页，腾出一个物理页：返回 1；找不到能换的（都在用、交换区满了）返回 0。见文件开头的说明。
 * 转两圈还没找到就算了：第一圈可能都是访问位为 1 的，清掉以后第二圈就有了。
 */
static int swap_out(void)
{
    for (uint32_t step = 0; step < 2u * NR_TASKS * 1024; step++) {
        if (clock_index == 1024) {
            clock_index = 0;
            clock_task = clock_task % (NR_TASKS - 1) + 1;
        }
        int is_current;
        uint32_t *table = sched_user_table(clock_task, &is_current);
        if (!table) {
            clock_index = 1024;     /* 这个任务没有用户空间：下一个 */
            continue;
        }
        uint32_t i = clock_index++;
        uint32_t flags = irq_save();
        uint32_t pte = table[i];
        uint32_t page = pte & ~0xFFFu;
        if (!(pte & PTE_PRESENT) || page_ref_count(page) != 1) {
            irq_restore(flags);
            continue;
        }
        if (pte & PTE_ACCESSED) {   /* 最近用过：清掉访问位，再给一次机会 */
            table[i] = pte & ~(uint32_t)PTE_ACCESSED;
            if (is_current)
                flush_tlb();
            irq_restore(flags);
            continue;
        }
        if (!(pte & PTE_DIRTY)) {   /* 没写过：扔掉，用到时再从程序文件读，或者再给一页 0 */
            table[i] = 0;
            if (is_current)
                flush_tlb();
            irq_restore(flags);
            free_page(page);
            return 1;
        }
        irq_restore(flags);
        /* 写过的：找一个空的交换页，页表项先改成换出去了，再写到硬盘上 */
        swap_lock();
        uint32_t slot = 0;
        while (slot < swap_total && swap_map[slot])
            slot++;
        flags = irq_save();
        int same = sched_user_table(clock_task, &is_current) == table && table[i] == pte && page_ref_count(page) == 1;
        if (slot == swap_total || !same) {      /* 交换区满了；或者等锁的时候这一页变了（主人用过它、结束了、fork 了） */
            irq_restore(flags);
            swap_unlock();
            if (slot == swap_total)
                return 0;
            continue;
        }
        swap_map[slot] = 1;
        swap_used++;
        table[i] = (slot + 1) << 12;
        if (is_current)
            flush_tlb();
        irq_restore(flags);
        int ok = swap_io(slot, page, 1) == 0;
        swap_unlock();
        if (!ok) {                  /* 写不进去：这一页不能丢，原样放回去（要是还没人动过） */
            flags = irq_save();
            if (table[i] == (slot + 1) << 12) {
                table[i] = pte;
                swap_map[slot] = 0;
                swap_used--;
            }
            irq_restore(flags);
            return 0;
        }
        free_page(page);
        return 1;
    }
    return 0;
}

/*
 * 空闲页少于 want，就换出去几页，直到够了或者再也换不出去。要在能睡、开着中断的地方调用。
 * 换出去的页里有当前任务自己的也不要紧：它用到时再缺页读回来。
 */
void mem_reclaim(uint32_t want)
{
    while (swap_total && free_page_count() < want && swap_out())
        ;
}
