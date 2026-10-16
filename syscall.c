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

typedef int32_t (*syscall_fn)(uint32_t a, uint32_t b, uint32_t c);

/* write(fd, buf, count)：目前只支持 fd 1，写到第 6 行的任务输出区。 */
static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t count)
{
    const char *p = (const char *)buf;

    if (fd != 1)
        return -1;
    for (uint32_t i = 0; i < count; i++)
        task_write_char(p[i]);
    return (int32_t)count;
}

static int32_t sys_getpid(uint32_t a, uint32_t b, uint32_t c)
{
    (void)a, (void)b, (void)c;
    return current_pid();
}

/*
 * sleep(ticks)：在内核里等待。陷阱门不清 IF，等待期间定时器照常中断，
 * 也可能在这里切换到别的任务；切换回来后接着等，直到时间到。
 */
static int32_t sys_sleep(uint32_t ticks, uint32_t b, uint32_t c)
{
    (void)b, (void)c;
    uint32_t start = timer_ticks;
    while (timer_ticks - start < ticks)
        __asm__ volatile("sti; hlt");
    return 0;
}

static const syscall_fn syscall_table[SYS_CALLS] = {
    [SYS_WRITE]  = sys_write,
    [SYS_GETPID] = sys_getpid,
    [SYS_SLEEP]  = sys_sleep,
};

void syscall_dispatch(struct interrupt_frame *frame)
{
    uint32_t nr = frame->eax;

    if (nr >= SYS_CALLS || syscall_table[nr] == 0) {
        frame->eax = (uint32_t)-1;  /* 未知的调用号 */
        return;
    }
    frame->eax = (uint32_t)syscall_table[nr](frame->ebx, frame->ecx, frame->edx);
}
