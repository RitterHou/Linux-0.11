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

/*
 * 前三个参数是调用方放在 EBX、ECX、EDX 里的值。最后一个参数是栈上保存的现场：
 * 从它的 CS 看出调用方是否在用户态（用户态传来的指针要检查），fork 还要复制它。
 */
typedef int32_t (*syscall_fn)(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame);

/*
 * write(fd, buf, count)：目前只支持 fd 1，写到第 6 行的任务输出区。
 * 用户态传来的 buf 必须完全落在用户程序的页内：内核能读所有内存，不检查的话，
 * 用户程序把内核地址交给 write，就能让内核替它把内核内存印出来。
 * Linux 0.11 不需要这一步：内核经 FS（用户数据段）用 get_fs_byte() 读用户内存，
 * 用户给的指针按用户段解释，越界由段界限挡住。这里的段都是平坦的，只能显式检查。
 */
static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t count, struct interrupt_frame *frame)
{
    const char *p = (const char *)buf;

    if (fd != 1)
        return -1;
    if (from_user_mode(frame) && !user_range_ok(buf, count))
        return -1;
    for (uint32_t i = 0; i < count; i++)
        task_write_char(p[i]);
    return (int32_t)count;
}

static int32_t sys_getpid(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)a, (void)b, (void)c, (void)frame;
    return current_pid();
}

/* sleep(ticks)：睡 ticks 个 tick，期间把 CPU 让给别的任务，见 sched.c 的 sched_sleep()。 */
static int32_t sys_sleep(uint32_t ticks, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    sched_sleep(ticks);
    return 0;
}

/* exit(code)：在第 7 行说明当前任务以什么代码结束，然后结束它，不再返回。 */
static int32_t sys_exit(uint32_t code, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)b, (void)c, (void)frame;
    struct text_line line;

    line_init(&line);
    line_append(&line, "task ");
    line_append_dec(&line, (uint32_t)current_pid());
    line_append(&line, " exited with code ");
    line_append_dec(&line, code);
    line_show(&line, VGA_YELLOW, TASK_MESSAGE_ROW);
    sched_exit(code & 0xFF);        /* 退出码只留低 8 位，与 Unix 相同 */
}

/* fork()：只有用户任务能 fork，内核任务 A 没有用户态的现场可以复制。 */
static int32_t sys_fork(uint32_t a, uint32_t b, uint32_t c, struct interrupt_frame *frame)
{
    (void)a, (void)b, (void)c;
    return from_user_mode(frame) ? sched_fork(frame) : -1;
}

/*
 * waitpid(pid, status, options)：等子进程结束，见 sched.c 的 sched_waitpid()。
 * status 可以为 0（不要状态）；不为 0 时内核要往那里写 4 个字节，用户传来的地址必须落在用户可写的内存里。
 */
static int32_t sys_waitpid(uint32_t pid, uint32_t status, uint32_t options, struct interrupt_frame *frame)
{
    if (status != 0 && from_user_mode(frame) && !user_write_ok(status, 4))
        return -1;
    return sched_waitpid((int32_t)pid, (uint32_t *)status, (int)options);
}

static const syscall_fn syscall_table[SYS_CALLS] = {
    [SYS_WRITE]  = sys_write,
    [SYS_GETPID] = sys_getpid,
    [SYS_SLEEP]  = sys_sleep,
    [SYS_EXIT]   = sys_exit,
    [SYS_FORK]   = sys_fork,
    [SYS_WAITPID] = sys_waitpid,
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
    frame->eax = (uint32_t)syscall_table[nr](frame->ebx, frame->ecx, frame->edx, frame);
}
