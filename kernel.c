/*
 * 内核的 C 入口：entry.asm 换好栈、清零 .bss 后调用 kmain()。
 *
 * 这里没有标准库，也没有 BIOS。kmain 先接着第 0 行的 ABPK 写 C 和问候语，
 * 然后装好 IDT、打开分页，再设置 8259 和定时器，打开中断，此后的工作都由中断驱动：
 * 定时器每秒 100 次刷新第 4 行的计数，键盘输入回显在第 6 行。
 */
#include "kernel.h"

#define TIMER_HZ 100                /* 与 Linux 0.11 的 HZ 相同，每 10 ms 一次 */

void kmain(void);

void kmain(void)
{
    vga_set_cursor(0, 4);           /* 第 0 行前 4 格已有 ABPK，从第 4 列接着写 */
    vga_putc('C');
    vga_puts("\n\nHello from kmain() in C!");

    gdt_init();                     /* 换上内核自己的 GDT，此后不再依赖 loader 所在的内存 */
    idt_init();                     /* 先装好 IDT：之后出了错（比如页表写错）才能显示异常 */

    paging_init();
    /* 通过虚拟地址 0xC0000000 写显存第 0 行第 5 列：它与 0xB8000 映射到同一块物理内存。 */
    ((volatile uint16_t *)VGA_ALIAS)[5] = (uint16_t)(VGA_GRAY << 8 | 'V');
    vga_write_at(5, 0, "paging: on, 0xC0000000 -> 0xB8000", VGA_GRAY);

    /* 顺序很重要：IDT 和 8259 都准备好之后，才能打开中断。 */
    pic_init();
    timer_init(TIMER_HZ);

    vga_write_at(4, 0, "ticks: 0", VGA_GRAY);
    vga_write_at(6, 0, "keyboard: ", VGA_GRAY);
    vga_set_cursor(6, 10);          /* 键盘回显从这里开始 */

    __asm__ volatile("sti");        /* 打开中断：定时器和键盘从此可以打断下面的循环 */
    vga_write_at(0, 6, "I", VGA_GRAY);

    selftest_run();                 /* 正常构建为空；make test 的自检内核在这里制造异常 */

    /* 没事可做就停下，等下一个中断唤醒 CPU；中断处理完又回到这里。 */
    for (;;)
        __asm__ volatile("hlt");
}
