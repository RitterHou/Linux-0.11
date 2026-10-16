/*
 * 内核的 C 入口：entry.asm 换好栈、清零 .bss 后调用 kmain()。
 *
 * 这里没有标准库，也没有 BIOS。kmain 先接着第 0 行的 ABPK 写 C 和问候语，
 * 然后装好 IDT、8259 和定时器，打开中断，此后的工作都由中断驱动：
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

    /* 顺序很重要：IDT 和 8259 都准备好之后，才能打开中断。 */
    idt_init();
    pic_init();
    timer_init(TIMER_HZ);

    vga_write_at(4, 0, "ticks: 0", VGA_GRAY);
    vga_write_at(6, 0, "keyboard: ", VGA_GRAY);
    vga_set_cursor(6, 10);          /* 键盘回显从这里开始 */

    __asm__ volatile("sti");        /* 打开中断：定时器和键盘从此可以打断下面的循环 */
    vga_write_at(0, 5, "I", VGA_GRAY);

    /* 没事可做就停下，等下一个中断唤醒 CPU；中断处理完又回到这里。 */
    for (;;)
        __asm__ volatile("hlt");
}
