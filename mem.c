/*
 * 物理页分配：用字节数组 mem_map 记录 1 MB～16 MB 之间每个 4 KB 物理页被引用了几次，0 表示空闲。
 * 做法与 Linux 0.11 的 mm/memory.c 相同：
 *   - LOW_MEM（1 MB）以下是内核自己的代码、数据和显存，不参与分配；
 *   - 内存假定为 16 MB：Makefile 给 QEMU 的是 -m 16M，paging.c 也只一一对应映射了前 16 MB，
 *     Linux 0.11 同样最多只管 16 MB；
 *   - get_free_page() 从高地址往低找空闲页，清零后交出去；free_page() 把引用次数减一。
 * 引用次数而不是“用了 / 没用”两种状态，是为以后 fork 时多个进程共用一页（写时复制）准备的。
 * Linux 0.11 把 1 MB～4 MB 留作磁盘缓冲区，主内存从 4 MB 开始；这里还没有磁盘缓冲区，全部用来分配。
 */
#include "io.h"
#include "kernel.h"

#define PAGING_PAGES ((HIGH_MEM - LOW_MEM) / PAGE_SIZE)     /* 3840 页 */
#define MAP_NR(addr) (((addr) - LOW_MEM) / PAGE_SIZE)

#define MEMORY_ROW   3              /* 第 3 行显示剩余的页数 */

static uint8_t mem_map[PAGING_PAGES];   /* 在 .bss，由 entry.asm 清零：开始时全部空闲 */
static uint32_t free_pages;

/* 第 3 行：还剩几页，缺页异常补上了几次、写时复制了几次、和别的进程共用了几次程序的页（见 paging.c）。 */
void mem_show(void)
{
    struct text_line line;

    line_init(&line);
    line_append(&line, "memory: ");
    line_append_dec(&line, free_pages);
    line_append(&line, "/");
    line_append_dec(&line, PAGING_PAGES);
    line_append(&line, " pages free  faults: ");      /* 写短一点：几个数都变大了也放得进 80 列 */
    line_append_dec(&line, page_faults);
    line_append(&line, "  cow: ");
    line_append_dec(&line, cow_faults);
    line_append(&line, "  shared: ");
    line_append_dec(&line, shared_pages);
    line_show(&line, VGA_GRAY, MEMORY_ROW);
}

void mem_init(void)
{
    free_pages = PAGING_PAGES;
    mem_show();
}

uint32_t free_page_count(void)
{
    return free_pages;
}

/*
 * 分配一个物理页，返回它的物理地址；没有空闲页时返回 0（0 永远不是能分配的页）。
 * 交出去之前先清零：页里可能还留着上一个使用者的数据。物理地址在一一对应映射的范围内，
 * 内核可以直接按物理地址写。关中断保证查找和标记之间不会被切走，两个任务拿到同一页。
 */
uint32_t get_free_page(void)
{
    uint32_t flags = irq_save();
    uint32_t page = 0;

    for (uint32_t i = PAGING_PAGES; i-- > 0;) {
        if (mem_map[i] == 0) {
            mem_map[i] = 1;
            free_pages--;
            page = LOW_MEM + i * PAGE_SIZE;
            break;
        }
    }
    irq_restore(flags);
    if (page == 0)
        return 0;

    volatile uint32_t *p = (volatile uint32_t *)page;
    for (int i = 0; i < PAGE_SIZE / 4; i++)
        p[i] = 0;
    mem_show();
    return page;
}

/* 又有一个页表项指向这一页：fork 时父子进程共用用户栈的页。 */
void page_add_ref(uint32_t addr)
{
    if (addr < LOW_MEM)
        return;
    uint32_t flags = irq_save();
    mem_map[MAP_NR(addr)]++;
    irq_restore(flags);
}

/* 这一页被几个页表项引用。写时复制时，只剩一个就不用复制。 */
uint32_t page_ref_count(uint32_t addr)
{
    return addr < LOW_MEM ? 0 : mem_map[MAP_NR(addr)];
}

/* 引用次数减一，减到 0 就回到空闲。释放一个本来就空闲的页说明内核自己记错了，只能停机。 */
void free_page(uint32_t addr)
{
    if (addr < LOW_MEM)             /* 内核自己的页不归这里管，Linux 0.11 也是直接返回 */
        return;
    if (addr >= HIGH_MEM)
        panic("trying to free nonexistent page");

    uint32_t flags = irq_save();
    uint8_t *count = &mem_map[MAP_NR(addr)];
    if (*count == 0) {
        irq_restore(flags);
        panic("trying to free free page");
    }
    if (--*count == 0)
        free_pages++;
    irq_restore(flags);
    mem_show();
}
