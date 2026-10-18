/*
 * 分页：建立页目录和页表，装入 CR3，再打开 CR0 的 PG 位。
 *
 * 打开分页后，CPU 访问的每个地址都是虚拟地址，要经过两级查表才得到物理地址：
 *   虚拟地址的高 10 位选页目录项 → 找到一张页表；中间 10 位选页表项 → 找到一个 4 KB 物理页；
 *   低 12 位是页内偏移。
 *
 * 这里把前 16 MB 一一对应映射（虚拟地址 = 物理地址），所以打开分页前后代码照常运行；
 * Linux 0.11 的 head.s 也是这样映射前 16 MB 的。另外有两处不同：
 *   - 第 0 页不映射，访问空指针会触发缺页异常，而不是悄悄读写物理地址 0；
 *   - 虚拟地址 0xC0000000 也映射到显存 0xB8000，同一块物理内存有了两个地址；
 *   - 只有用户程序所在的几页（链接脚本中的 __user_start～__user_end）打开“用户”位，
 *     特权级 3 的代码访问其他页都会触发缺页异常；其中代码所在的页对用户态只读。
 */
#include "kernel.h"

#define PAGE_PRESENT  0x001         /* 存在位：为 0 时访问会触发 14 号缺页异常 */
#define PAGE_WRITE    0x002         /* 可写 */
#define PAGE_USER     0x004         /* 用户：为 0 时只有特权级 0～2 能访问 */
#define CR0_PG        0x80000000u

#define ENTRIES       1024          /* 每张页目录或页表有 1024 项，每项 4 字节 */
#define IDENTITY_MB   16
#define IDENTITY_TABLES (IDENTITY_MB / 4)   /* 每张页表管 4 MB */

/* mem.c 按物理地址直接清零分出去的页，所以能分配的页都必须在一一对应映射的范围内。 */
_Static_assert(HIGH_MEM <= IDENTITY_MB * 0x100000u, "allocatable pages must be identity-mapped");

/* 都在 .bss，由 entry.asm 清零，所以没有填写的项存在位都是 0。CR3 要求 4 KB 对齐。 */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t identity_tables[IDENTITY_TABLES][ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t alias_table[ENTRIES] __attribute__((aligned(PAGE_SIZE)));

extern uint8_t __user_start[], __user_data[], __user_end[];   /* 链接脚本定义，都按 4 KB 对齐 */

void paging_init(void)
{
    /*
     * 页目录项和页表项都有用户位，CPU 取两者的“与”：页目录项打开用户位，
     * 才轮到页表项决定每一页。内核的页在页表项中不开用户位，用户态照样访问不了。
     */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        for (int i = 0; i < ENTRIES; i++) {
            uint32_t phys = (uint32_t)(t * ENTRIES + i) * PAGE_SIZE;
            identity_tables[t][i] = phys | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[t] = (uint32_t)identity_tables[t] | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    identity_tables[0][0] = 0;      /* 第 0 页不映射，用来捕获空指针 */

    /*
     * 用户程序的页打开用户位。它们在前 4 MB 内，属于第一张页表。
     * 代码页去掉可写位：用户栈溢出或野指针写到代码上，会立刻触发缺页，而不是悄悄改掉代码。
     * 386 在特权级 0 不理会只读位（CR0.WP 为 0），内核仍可写这些页，但内核从不写用户代码。
     */
    for (uint32_t addr = (uint32_t)__user_start; addr < (uint32_t)__user_end; addr += PAGE_SIZE) {
        uint32_t *pte = &identity_tables[0][addr / PAGE_SIZE];
        *pte |= PAGE_USER;
        if (addr < (uint32_t)__user_data)
            *pte &= ~PAGE_WRITE;
    }

    /* 0xC0000000 的高 10 位是 768，用第 768 个页目录项；它的第 0 页指向显存。 */
    alias_table[0] = VGA_PHYS | PAGE_PRESENT | PAGE_WRITE;
    page_directory[VGA_ALIAS >> 22] = (uint32_t)alias_table | PAGE_PRESENT | PAGE_WRITE;

    /* 页表在一一对应映射的范围内，它们的虚拟地址就是物理地址，可以直接装入 CR3。 */
    __asm__ volatile("movl %0, %%cr3" : : "r"(page_directory) : "memory");

    uint32_t cr0;
    __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movl %0, %%cr0" : : "r"(cr0 | CR0_PG) : "memory");
}

/* [addr, addr + size) 是否完全落在用户程序的页内。系统调用用它检查用户传来的指针。 */
int user_range_ok(uint32_t addr, uint32_t size)
{
    uint32_t start = (uint32_t)__user_start, end = (uint32_t)__user_end;
    return addr >= start && addr <= end && size <= end - addr;
}
