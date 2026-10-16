/* 内核各模块共用的声明。 */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#define VGA_GRAY      0x07          /* 黑底浅灰字 */
#define VGA_WHITE_RED 0x4F          /* 红底白字，用于异常信息 */

/* vga.c：直接写 VGA 文本显存 */
void vga_set_cursor(int row, int col);
void vga_putc(char ch);
void vga_puts(const char *s);
void vga_write_at(int row, int col, const char *s, uint8_t color);
char *format_uint(uint32_t value, unsigned base, char buf[12]);

/*
 * 中断发生时栈上的内容，从低地址到高地址排列。
 * isr.asm 依次压入错误码（或补 0）、中断号和 pushad 的 8 个寄存器，
 * 更早的 EIP、CS、EFLAGS 由 CPU 自动压入。
 */
struct interrupt_frame {
    uint32_t edi, esi, ebp, esp_at_pushad, ebx, edx, ecx, eax;   /* pushad */
    uint32_t vector;                                            /* isr.asm 压入 */
    uint32_t error_code;                                        /* CPU 压入或补 0 */
    uint32_t eip, cs, eflags;                                   /* CPU 压入 */
};

/* interrupt.c：IDT、8259 与中断分发 */
void idt_init(void);
void pic_init(void);
void interrupt_dispatch(struct interrupt_frame *frame);

/* timer.c：8253 定时器 */
extern volatile uint32_t timer_ticks;
void timer_init(uint32_t hz);
void timer_handle(void);

/* keyboard.c：键盘 */
void keyboard_handle(void);

/* selftest.c：make test 构建的自检内核在这里故意制造异常 */
void selftest_run(void);

#endif
