/* 内核各模块共用的声明。 */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#define VGA_GRAY      0x07          /* 黑底浅灰字 */
#define VGA_WHITE_RED 0x4F          /* 红底白字，用于异常信息 */
#define VGA_YELLOW    0x0E          /* 黑底黄字，用于任务结束的消息 */

/* gdt.c：内核自己的 GDT，选择子与 loader 的相同 */
#define KERNEL_CODE_SEL 0x08
#define KERNEL_DATA_SEL 0x10
#define USER_CODE_SEL   0x33        /* GDT 第 6 项，低 2 位的 RPL 为 3 */
#define USER_DATA_SEL   0x3B        /* GDT 第 7 项 */
void gdt_init(void);
void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags);

/* vga.c：直接写 VGA 文本显存 */
void vga_set_window(int top, int bottom);
void vga_set_cursor(int row, int col);
void vga_putc(char ch);
void vga_puts(const char *s);
void vga_write_at(int row, int col, const char *s, uint8_t color);
char *format_uint(uint32_t value, unsigned base, char buf[12]);

/* vga.c：拼一整行文字再写出 */
struct text_line {
    char text[81];
    int len;
};
void line_init(struct text_line *line);
void line_append(struct text_line *line, const char *s);
void line_append_dec(struct text_line *line, uint32_t value);
void line_append_hex(struct text_line *line, const char *name, uint32_t value, int digits);
void line_show(struct text_line *line, uint8_t color, int row);

/*
 * 中断发生时栈上的内容，从低地址到高地址排列。
 * isr.asm 依次压入错误码（或补 0）、中断号、pushad 的 8 个寄存器和 4 个段寄存器，
 * 更早的 EIP、CS、EFLAGS 由 CPU 自动压入。段寄存器压栈时占 4 字节，只有低 16 位有效。
 * 从用户态（CS 的低 2 位为 3）进入时，CPU 先换到 TSS 中的内核栈，再多压入用户态的 ESP 和 SS；
 * 从内核态进入时没有这两项，user_esp、user_ss 的位置是被打断代码的栈内容。
 */
struct interrupt_frame {
    uint32_t gs, fs, es, ds;                                    /* isr.asm 压入 */
    uint32_t edi, esi, ebp, esp_at_pushad, ebx, edx, ecx, eax;   /* pushad */
    uint32_t vector;                                            /* isr.asm 压入 */
    uint32_t error_code;                                        /* CPU 压入或补 0 */
    uint32_t eip, cs, eflags;                                   /* CPU 压入 */
    uint32_t user_esp, user_ss;                                 /* 仅从用户态进入时 CPU 才压入 */
};

static inline int from_user_mode(const struct interrupt_frame *frame)
{
    return (frame->cs & 3) == 3;
}

/* interrupt.c：IDT、8259 与中断分发 */
void idt_init(void);
void pic_init(void);
void interrupt_dispatch(struct interrupt_frame *frame);
void panic(const char *msg) __attribute__((noreturn));

/* paging.c：页目录与页表 */
#define PAGE_SIZE 4096
#define VGA_PHYS  0x000B8000u       /* 显存的物理地址 */
#define VGA_ALIAS 0xC0000000u       /* 打开分页后，这个虚拟地址也指向显存 */
/*
 * 用户栈所在的 4 MB（第 256 个页目录项）。开始时一页也没有映射，任务 B 用到哪一页，
 * 缺页异常就给哪一页分配物理内存；任务结束时全部收回。栈从顶端往下长。
 */
#define USER_STACK_LOW 0x40000000u
#define USER_STACK_TOP 0x40400000u
void paging_init(void);
int user_range_ok(uint32_t addr, uint32_t size);
int handle_page_fault(uint32_t addr, uint32_t error_code);
uint32_t kernel_page_dir(void);
uint32_t new_page_dir(void);
void free_page_dir(uint32_t dir);
extern uint32_t page_faults;

/* mem.c：物理页分配，管 LOW_MEM～HIGH_MEM 之间的页 */
#define LOW_MEM   0x00100000u       /* 1 MB 以下归内核自己 */
#define HIGH_MEM  0x01000000u       /* 内存假定为 16 MB */
void mem_init(void);
uint32_t get_free_page(void);
void free_page(uint32_t addr);
uint32_t free_page_count(void);
void mem_show(void);

/* timer.c：8253 定时器 */
#define TIMER_HZ 100                /* 与 Linux 0.11 的 HZ 相同，每 10 ms 一次 */
extern volatile uint32_t timer_ticks;
void timer_init(uint32_t hz);
void timer_handle(void);

/* sched.c：任务表、任务切换、睡眠与唤醒 */
#define NR_TASKS      16            /* 任务槽的个数，槽号就是任务号 */
#define TSS_GDT_INDEX 10            /* 任务 n 的 TSS 描述符在 GDT 第 10 + n 项 */
#define TSS_SEL(n)    ((TSS_GDT_INDEX + (n)) * 8)
void sched_init(void);
void sched_start(void);
void schedule(void);
void sched_sleep(uint32_t ticks);
int current_pid(void);
void task_write_char(char ch);
void sched_exit_current(void);
#define TASK_MESSAGE_ROW 7          /* 任务结束的消息写在第 7 行 */

/* syscall.c：int 0x80 系统调用 */
void syscall_dispatch(struct interrupt_frame *frame);

/* keyboard.c：键盘 */
void keyboard_init(void);
void keyboard_handle(void);
void keyboard_process(uint8_t scancode);

/* user.c：任务 B 在用户态运行的程序 */
void user_main(void);

/* selftest.c：make test 构建的自检内核在这里故意制造异常 */
void selftest_run(void);
void selftest_early(void);

#endif
