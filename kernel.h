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
void pic_unmask(int irq);

/* paging.c：页目录与页表 */
#define PAGE_SIZE 4096
#define VGA_PHYS  0x000B8000u       /* 显存的物理地址 */
#define VGA_ALIAS 0xC0000000u       /* 打开分页后，这个虚拟地址也指向显存 */
/*
 * 用户空间：第 256 个页目录项管的 4 MB，每个用户任务各有一份。从低往高是：exec 放的程序，堆（往上长到 brk），
 * 一段没要过的空地，顶端 64 KB 的栈区（栈从顶端往下长）。程序、堆和栈区里没有映射的页在第一次用到时由缺页异常分配，
 * 空地里的不分；任务结束时全部收回。
 */
#define USER_LOW 0x40000000u
#define USER_TOP 0x40400000u
#define STACK_RESERVE (64 * 1024u)  /* 用户空间顶端的 64 KB 留给栈：程序和堆都不能伸进来 */
#define STACK_LOW (USER_TOP - STACK_RESERVE)
#define HEAP_GUARD (16 * 1024u)     /* 堆最高也离栈区 16 KB：栈用超了碰到的是空地，进程被结束，而不是悄悄写坏堆 */
#define BRK_LIMIT (STACK_LOW - HEAP_GUARD)
void paging_init(void);
int user_range_ok(uint32_t addr, uint32_t size);
int user_write_ok(uint32_t addr, uint32_t size);
int copy_string(char *dst, uint32_t src, uint32_t max, int from_user);
#define PATH_MAX 128                /* 路径最长 127 个字节（相对路径接上当前目录之后） */
int handle_page_fault(uint32_t addr, uint32_t error_code, int can_sleep);
uint32_t user_page_phys(uint32_t dir, uint32_t addr);
uint32_t kernel_page_dir(void);
uint32_t new_page_dir(void);
void free_page_dir(uint32_t dir);
uint32_t copy_page_dir(uint32_t src);
uint32_t map_user_page(uint32_t dir, uint32_t addr, int writable);
extern uint32_t page_faults, cow_faults, shared_pages;

/* mem.c：物理页分配，管 LOW_MEM～HIGH_MEM 之间的页 */
#define LOW_MEM   0x00100000u       /* 1 MB 以下归内核自己 */
#define HIGH_MEM  0x01000000u       /* 内存假定为 16 MB */
void mem_init(void);
uint32_t get_free_page(void);
void free_page(uint32_t addr);
void page_add_ref(uint32_t addr);
uint32_t page_ref_count(uint32_t addr);
uint32_t free_page_count(void);
void mem_show(void);

/* timer.c：8253 定时器 */
#define TIMER_HZ 100                /* 与 Linux 0.11 的 HZ 相同，每 10 ms 一次 */
extern volatile uint32_t timer_ticks;
void timer_init(uint32_t hz);
void time_init(void);
uint32_t current_time(void);
void clock_show(void);
void timer_handle(int user);

/* sched.c：任务表、任务切换、睡眠与唤醒 */
#define NR_TASKS      16            /* 任务槽的个数，槽号就是任务号 */
#define TSS_GDT_INDEX 10            /* 任务 n 的 TSS 描述符在 GDT 第 10 + n 项 */
#define TSS_SEL(n)    ((TSS_GDT_INDEX + (n)) * 8)
void sched_init(void);
void sched_start(void);
void schedule(int user);
void sched_sleep(uint32_t ticks);
int current_pid(void);
void task_write_char(char ch);
void sched_exit(uint32_t status) __attribute__((noreturn));
void sched_idle_wakeup(void);
struct file **current_files(void);
char *current_cwd(void);
uint32_t *current_cloexec(void);

/* 等待队列：第 n 位为 1 表示任务 n 睡在上面。调用 sleep_on 之前要关中断，检查条件和睡下之间不能被打断。 */
struct wait_queue {
    uint32_t sleepers;
};
void sleep_on(struct wait_queue *q);
int interruptible_sleep_on(struct wait_queue *q);
void wake_up(struct wait_queue *q);
int sched_kill(int pid, int sig);
int sched_signal(int sig, uint32_t handler, uint32_t restorer);
int sched_setsid(void);
int sched_pgrp(void);
int sched_is_leader(void);
int sched_claim_tty(int tty);
int sched_take_signal(uint32_t *handler, uint32_t *restorer);
void sched_set_brk(uint32_t end);
uint32_t sched_alarm(uint32_t seconds);
int sched_pause(void);
uint32_t sched_brk(uint32_t addr);
uint32_t current_brk(void);
void sched_exec_signals(void);
void signal_current(int sig);
int sched_fork(const struct interrupt_frame *frame);
int sched_waitpid(int pid, uint32_t *status, int options);
uint32_t sched_set_page_dir(uint32_t dir);
void sched_set_name(const char *path);
struct taskinfo;
int sched_taskinfo(int pid, struct taskinfo *info);
int sched_nice(int inc);
/* 用户号和组号：实际的（是谁）、有效的（检查权限用）、保存的（setuid 程序换掉有效的以后还能换回来） */
#define ID_UID  0
#define ID_EUID 1
#define ID_GID  2
#define ID_EGID 3
uint32_t sched_id(int which);
int suser(void);                    /* 有效用户号是 0（root） */
int sched_setuid(uint32_t uid);
int sched_setgid(uint32_t gid);
void sched_exec_ids(int set_uid, uint32_t uid, int set_gid, uint32_t gid);
struct tms;
uint32_t sched_times(struct tms *buf);
int do_exec(const char *path, uint32_t user_argv, struct interrupt_frame *frame);
/* 程序的一个要装入的段：exec 时记下，缺页时照着从文件里读 */
#define MAX_SEGS 4
struct segment {
    uint32_t vaddr, memsz;          /* 在用户空间里占 [vaddr, vaddr + memsz) */
    uint32_t offset, filesz;        /* 文件里的 [offset, offset + filesz)，后面到 memsz 是 .bss，补 0 */
    int writable;
};
struct m_inode;
struct m_inode *sched_set_exec(struct m_inode *ip, const struct segment *segs, int n);
struct m_inode *current_exec(const struct segment **segs, int *n);
uint32_t sched_share_page(struct m_inode *ip, uint32_t va);
void sched_drop_exec(void);
int exec_fill_page(uint32_t va, uint32_t page, int *writable);
#define EXIT_KILLED 0x100           /* 被异常结束时留给父进程的状态：0x100 加异常号 */
#define EXIT_SIGNAL 0x200           /* 被信号结束时的状态：0x200 加信号编号 */
#define TASK_MESSAGE_ROW 7          /* 任务结束的消息写在第 7 行 */

/* syscall.c：int 0x80 系统调用 */
void syscall_dispatch(struct interrupt_frame *frame);

/* hd.c：硬盘 */
extern uint32_t hd_sectors[2], hd_requests;
void hd_init(void);
void hd_handle(void);
int hd_read(int drive, uint32_t lba, void *buf);
int hd_write(int drive, uint32_t lba, const void *buf);

/* tty.c：终端 */
#define NR_TTYS     2
#define TTY_CONSOLE 0               /* 键盘和第 8～18 行的窗口：/dev/tty0 */
#define TTY_SERIAL  1               /* 串口 COM1：/dev/ttyS0 */
void tty_input(int tty, char ch);
void tty_set_pgrp(int tty, int pgrp);
void tty_release_pgrp(int tty, int pgrp);
int32_t tty_read(int tty, void *dst, uint32_t n);
int32_t tty_write(int tty, const void *src, uint32_t n);
struct termios;
int tty_ioctl(int tty, uint32_t cmd, struct termios *t);

/* serial.c：串口 */
void serial_init(void);
int serial_present(void);
void serial_handle(void);
void serial_putc(char ch);

/* keyboard.c：键盘 */
void keyboard_init(void);
void keyboard_handle(void);
void keyboard_process(uint8_t scancode);

/* user.c：任务 B 在用户态运行的程序 */
void user_main(void);

/* selftest.c：make test 构建的自检内核在这里故意制造异常 */
void selftest_run(void);
void selftest_early(void);
void selftest_task_a(void);

#endif
