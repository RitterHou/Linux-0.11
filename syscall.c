/*
 * 系统调用的内核一侧。
 *
 * int 0x80 经 IDT 第 0x80 项（DPL 3 的陷阱门）进入 isr.asm 的 isr_syscall，
 * 和其他中断一样整理好栈后交给 interrupt_dispatch()，再转到这里的 syscall_dispatch()：
 * 按 EAX 中的调用号查表，把 EBX、ECX、EDX 作为参数，返回值写回栈上保存的 EAX，
 * 中断返回时 popad 恢复寄存器，调用方就在 EAX 里拿到了返回值。
 * Linux 0.11 的 kernel/system_call.s 用同样的方式查 sys_call_table。
 */
#include "kernel.h"
#include "syscall.h"

/* 最后一个参数表示调用方是否在用户态，用户态传来的指针要检查。 */
typedef int32_t (*syscall_fn)(uint32_t a, uint32_t b, uint32_t c, int from_user);

/*
 * write(fd, buf, count)：目前只支持 fd 1，写到第 6 行的任务输出区。
 * 用户态传来的 buf 必须完全落在用户程序的页内：内核能读所有内存，不检查的话，
 * 用户程序把内核地址交给 write，就能让内核替它把内核内存印出来。
 * Linux 0.11 在 sys_write 等处用 verify_area() 检查用户地址。
 */
static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t count, int from_user)
{
    const char *p = (const char *)buf;

    if (fd != 1)
        return -1;
    if (from_user && !user_range_ok(buf, count))
        return -1;
    for (uint32_t i = 0; i < count; i++)
        task_write_char(p[i]);
    return (int32_t)count;
}

static int32_t sys_getpid(uint32_t a, uint32_t b, uint32_t c, int from_user)
{
    (void)a, (void)b, (void)c, (void)from_user;
    return current_pid();
}

/*
 * sleep(ticks)：在内核里等待。陷阱门不清 IF，等待期间定时器照常中断，
 * 也可能在这里切换到别的任务；切换回来后接着等，直到时间到。
 */
static int32_t sys_sleep(uint32_t ticks, uint32_t b, uint32_t c, int from_user)
{
    (void)b, (void)c, (void)from_user;
    uint32_t start = timer_ticks;
    while (timer_ticks - start < ticks)
        __asm__ volatile("sti; hlt");
    return 0;
}

/* exit(code)：在第 7 行说明当前任务以什么代码结束，然后结束它，不再返回。 */
static int32_t sys_exit(uint32_t code, uint32_t b, uint32_t c, int from_user)
{
    (void)b, (void)c, (void)from_user;
    struct text_line line;

    line_init(&line);
    line_append(&line, "task ");
    line_append_dec(&line, (uint32_t)current_pid());
    line_append(&line, " exited with code ");
    line_append_dec(&line, code);
    line_show(&line, VGA_YELLOW, TASK_MESSAGE_ROW);
    sched_exit_current();
    return 0;                       /* 不会执行到这里 */
}

static const syscall_fn syscall_table[SYS_CALLS] = {
    [SYS_WRITE]  = sys_write,
    [SYS_GETPID] = sys_getpid,
    [SYS_SLEEP]  = sys_sleep,
    [SYS_EXIT]   = sys_exit,
};

void syscall_dispatch(struct interrupt_frame *frame)
{
    static int user_call_seen;
    uint32_t nr = frame->eax;
    int from_user = from_user_mode(frame);

    if (from_user && !user_call_seen) {
        user_call_seen = 1;
        vga_write_at(0, 8, "U", VGA_GRAY);  /* 第一次收到来自特权级 3 的系统调用 */
    }

    if (nr >= SYS_CALLS || syscall_table[nr] == 0) {
        frame->eax = (uint32_t)-1;  /* 未知的调用号 */
        return;
    }
    frame->eax = (uint32_t)syscall_table[nr](frame->ebx, frame->ecx, frame->edx, from_user);
}
