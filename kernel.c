/*
 * 内核的 C 入口：entry.asm 换好栈、清零 .bss 后调用 kmain()。
 *
 * 这里没有标准库，也没有 BIOS。kmain 先接着第 0 行的 ABPK 写 C 和问候语，
 * 然后装好 IDT、打开分页，再设置 8259 和定时器，打开中断，此后的工作都由中断驱动：
 * 定时器每秒 100 次刷新第 4 行的计数并轮流运行两个任务，键盘输入回显在第 8～18 行。
 */
#include "kernel.h"

#define CONSOLE_TOP    8            /* 键盘回显窗口：第 8～18 行，写满后向上滚动 */
#define CONSOLE_BOTTOM 18

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

    selftest_early();               /* 正常构建为空；自检 EARLY_KEY 在这里等一次按键 */

    /* 顺序很重要：IDT 和 8259 都准备好之后，才能打开中断。 */
    pic_init();
    keyboard_init();                /* 读光开中断前就到达的按键，否则键盘中断可能再也不来 */
    timer_init(TIMER_HZ);
    sched_init();                   /* 建好两个任务的 TSS，把 kmain 登记为任务 0 */

    vga_write_at(4, 0, "ticks: 0", VGA_GRAY);
    vga_write_at(6, 0, "tasks: ", VGA_GRAY);
    vga_set_window(CONSOLE_TOP, CONSOLE_BOTTOM);
    vga_set_cursor(CONSOLE_TOP, 0);
    vga_puts("keyboard: ");         /* 键盘回显接在后面；滚动时这一行也会上移 */

    __asm__ volatile("sti");        /* 打开中断：定时器和键盘从此可以打断下面的循环 */
    vga_write_at(0, 6, "I", VGA_GRAY);

    /* 正常构建为空；make test 的自检内核在这里制造异常。必须在开始调度之前：
       有的自检要让 kmain 一直停在这里，开始调度后 kmain 只在任务 A、B 都睡着时才运行。 */
    selftest_run();

    sched_start();                  /* 下一个定时器中断起，任务 A、B 轮流运行 */
    vga_write_at(0, 7, "T", VGA_GRAY);

    /*
     * kmain 从此是任务 0，也就是空闲任务：任务 A、B 都在睡眠时，调度器切回这里，CPU 停在 hlt 上，
     * 下一个定时器中断把到时间的任务叫醒，再切过去。Linux 0.11 的 main() 最后是 for(;;) pause();，
     * 任务 0 同样只在没有别的进程能运行时才运行。
     */
    for (;;)
        __asm__ volatile("hlt");
}
