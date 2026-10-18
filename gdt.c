/*
 * 内核自己的 GDT。
 *
 * loader 进入保护模式时用的 GDT 写在 loader.bin 里，位于 0x8000 附近，只适合临时使用：
 * 那块内存不属于内核，以后也放不下 TSS 等新的描述符。内核在这里建一份新的，
 * 选择子与 loader 的保持一致（0x08 代码段、0x10 数据段），所以换表前后段寄存器的值不变。
 * Linux 0.11 的 head.s 也会重建 GDT，因为 setup.s 留下的那份在 0x90000 附近，之后会被覆盖。
 */
#include "kernel.h"

/* 0 空，1～2 内核段，6～7 用户段，9 留给自检，10 起每个任务槽一个 TSS；3～5、8 空着。 */
#define GDT_ENTRIES (TSS_GDT_INDEX + NR_TASKS)

struct gdt_pointer {                /* lgdt 的操作数：界限和基址，共 6 字节 */
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 位于 .bss，由 entry.asm 清零，第 0 项天然就是空描述符。 */
static uint64_t gdt[GDT_ENTRIES] __attribute__((aligned(8)));

/*
 * 按字段拼出一个 8 字节段描述符。基址和界限被拆散在描述符的不同位置，这是 80286 留下的格式。
 *   access：存在位、DPL、类型；flags：粒度（1 表示界限以 4 KB 为单位）和 32 位标志。
 */
static uint64_t make_descriptor(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    return (uint64_t)(limit & 0xFFFF)
         | (uint64_t)(base & 0xFFFFFF) << 16
         | (uint64_t)access << 40
         | (uint64_t)((limit >> 16) & 0xF) << 48
         | (uint64_t)(flags & 0xF) << 52
         | (uint64_t)(base >> 24) << 56;
}

/* 填写第 index 项。选择子 = index × 8。 */
void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt[index] = make_descriptor(base, limit, access, flags);
}

void gdt_init(void)
{
    /* 与 loader.asm 中的两项相同：基址 0、界限 0xFFFFF × 4 KB = 4 GB 的平坦段。 */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC);    /* 代码段：存在、DPL 0、可执行可读 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC);    /* 数据段：存在、DPL 0、可读写 */
    /* 用户段：范围相同，只是 DPL 为 3，特权级 3 的代码才能使用。能访问哪些内存由页表的用户位决定。 */
    gdt_set_entry(6, 0, 0xFFFFF, 0xFA, 0xC);    /* 用户代码段：存在、DPL 3、可执行可读 */
    gdt_set_entry(7, 0, 0xFFFFF, 0xF2, 0xC);    /* 用户数据段：存在、DPL 3、可读写 */

    struct gdt_pointer gdtr = { sizeof(gdt) - 1, (uint32_t)gdt };
    __asm__ volatile("lgdt %0" : : "m"(gdtr));

    /*
     * 段寄存器里缓存着装入时读到的描述符，换了 GDT 也不会自动更新，要重新装载一次。
     * CS 只能用远跳转装载，其余段寄存器直接 mov。
     */
    __asm__ volatile(
        "ljmp %0, $1f\n"
        "1:\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss"
        : : "i"(KERNEL_CODE_SEL), "i"(KERNEL_DATA_SEL) : "eax", "memory");
}
