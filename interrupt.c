/*
 * IDT、8259 中断控制器与中断分发。
 *
 * CPU 收到中断或发生异常时，用中断号在 IDT 中查到入口地址（isr.asm 中的 isrN），
 * 入口整理好栈后调用 interrupt_dispatch()，再按中断号交给具体的处理函数。
 */
#include "io.h"
#include "kernel.h"
#include "syscall.h"

#define IDT_ENTRIES      256
#define ISR_STUBS        48         /* isr.asm 提供 0～47 号入口 */
#define INTERRUPT_GATE   0x8E       /* 存在、DPL 0、32 位中断门：进入时 CPU 自动清 IF */
#define USER_TRAP_GATE   0xEF       /* 存在、DPL 3、32 位陷阱门：用户态也能 int 调用，进入时不清 IF */

#define PIC1_CMD   0x20             /* 主片：IRQ0～7 */
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0             /* 从片：IRQ8～15，接在主片的 IRQ2 上 */
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

#define IRQ_BASE      0x20          /* 主片 IRQ0～7 映射到 0x20～0x27 */
#define IRQ_BASE_PIC2 0x28          /* 从片 IRQ8～15 映射到 0x28～0x2F */
#define IRQ_TIMER     (IRQ_BASE + 0)
#define IRQ_KEYBOARD  (IRQ_BASE + 1)

struct idt_entry {
    uint16_t offset_low;            /* 入口地址低 16 位 */
    uint16_t selector;              /* 入口所在的代码段 */
    uint8_t  zero;
    uint8_t  type_attr;             /* 门类型、DPL、存在位 */
    uint16_t offset_high;           /* 入口地址高 16 位 */
} __attribute__((packed));

struct idt_pointer {                /* lidt 的操作数：界限和基址，共 6 字节 */
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

extern const uint32_t isr_stub_table[ISR_STUBS];
extern void isr_syscall(void);

/* 位于 .bss，由 entry.asm 清零；没有填写的表项存在位为 0。 */
static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(8)));

static void idt_set_gate(int vector, uint32_t handler, uint8_t type_attr)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = KERNEL_CODE_SEL;
    idt[vector].zero        = 0;
    idt[vector].type_attr   = type_attr;
    idt[vector].offset_high = handler >> 16;
}

void idt_init(void)
{
    for (int i = 0; i < ISR_STUBS; i++)
        idt_set_gate(i, isr_stub_table[i], INTERRUPT_GATE);

    /*
     * 系统调用用 DPL 3 的门：int n 只能调用 DPL ≥ 当前特权级的门，DPL 0 的门在用户态调用会触发 #GP。
     * 用陷阱门而不是中断门，系统调用执行期间中断仍然打开，sleep 才能等到定时器。
     * Linux 0.11 用 set_system_gate(0x80, &system_call) 做同样的设置。
     */
    idt_set_gate(SYSCALL_VECTOR, (uint32_t)isr_syscall, USER_TRAP_GATE);

    /*
     * 48～255 号表项全为 0，类型字段 0 不是合法的门描述符。万一被触发，CPU 在检查
     * 存在位之前就会产生 13 号一般保护异常，错误码为 中断号 × 8 + 2，同样能显示出来。
     */
    struct idt_pointer idtr = { sizeof(idt) - 1, (uint32_t)idt };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

/*
 * BIOS 把主片的 IRQ0～7 映射到 0x08～0x0F，与 CPU 异常 8～15 重叠：
 * 不改的话，每次定时器中断都会被当成 8 号双重错误。
 * 所以重新初始化两片 8259，把 IRQ 挪到 0x20～0x2F，这与 Linux 0.11 的 setup.s 相同。
 */
void pic_init(void)
{
    outb(PIC1_CMD, 0x11);  io_wait();       /* ICW1：边沿触发、级联、需要 ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, IRQ_BASE);      io_wait();  /* ICW2：起始中断号 */
    outb(PIC2_DATA, IRQ_BASE_PIC2); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();       /* ICW3：主片的 IRQ2 接着从片 */
    outb(PIC2_DATA, 0x02); io_wait();       /*       从片的级联号是 2 */
    outb(PIC1_DATA, 0x01); io_wait();       /* ICW4：8086 模式，需要软件发 EOI */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFC);                  /* 屏蔽字：位为 1 表示屏蔽，只打开 IRQ0 和 IRQ1 */
    outb(PIC2_DATA, 0xFF);                  /* 从片全部屏蔽 */
}

/* 告诉 8259 这个中断已处理完。不发 EOI，同级及更低优先级的 IRQ 就不会再送来。 */
static void pic_send_eoi(uint32_t vector)
{
    if (vector >= IRQ_BASE_PIC2)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

static const char *const exception_names[20] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow",
    "#BR bound range", "#UD invalid opcode", "#NM no FPU", "#DF double fault",
    "coprocessor overrun", "#TS invalid TSS", "#NP segment not present",
    "#SS stack fault", "#GP general protection", "#PF page fault", "reserved",
    "#MF FPU error", "#AC alignment check", "#MC machine check", "#XM SIMD error",
};

/* 异常面板占第 19～23 行，在键盘回显窗口（第 8～18 行）下面，滚动时不会被移走。 */
#define PANEL_ROW 19

/*
 * 内核态发生的 CPU 异常无法恢复：在第 19～23 行用红底白字显示异常号和出错时的全部寄存器，然后停机。
 * 寄存器的值来自栈上的 struct interrupt_frame，也就是异常发生那一刻的状态。
 */
static void exception_panic(const struct interrupt_frame *frame)
{
    char buf[12];
    const char *name = frame->vector < 20 ? exception_names[frame->vector] : "reserved";
    uint32_t cr2, cr3;
    struct text_line line;

    __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));     /* 缺页时 CPU 把出错的虚拟地址放在这里 */
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
    /*
     * 出错时的栈：从用户态进入时，CPU 压入了用户态的 SS 和 ESP；
     * 同一特权级内发生异常时 CPU 不压入它们，出错时的 ESP 就是 EFLAGS 之上的地址。
     */
    uint32_t esp, ss;
    if (from_user_mode(frame)) {
        esp = frame->user_esp;
        ss = frame->user_ss;
    } else {
        esp = (uint32_t)&frame->user_esp;
        __asm__ volatile("movl %%ss, %0" : "=r"(ss));
    }

    line_init(&line);
    line_append(&line, "Exception ");
    line_append(&line, format_uint(frame->vector, 10, buf));
    line_append(&line, " ");
    line_append(&line, name);
    line_show(&line, VGA_WHITE_RED, PANEL_ROW);

    line_append_hex(&line, "EIP", frame->eip, 8);
    line_append_hex(&line, "CS", frame->cs, 4);
    line_append_hex(&line, "EFLAGS", frame->eflags, 8);
    line_append_hex(&line, "error", frame->error_code, 8);
    line_append_hex(&line, "SS", ss, 4);
    line_show(&line, VGA_WHITE_RED, PANEL_ROW + 1);

    line_append_hex(&line, "EAX", frame->eax, 8);
    line_append_hex(&line, "EBX", frame->ebx, 8);
    line_append_hex(&line, "ECX", frame->ecx, 8);
    line_append_hex(&line, "EDX", frame->edx, 8);
    line_show(&line, VGA_WHITE_RED, PANEL_ROW + 2);

    line_append_hex(&line, "ESI", frame->esi, 8);
    line_append_hex(&line, "EDI", frame->edi, 8);
    line_append_hex(&line, "EBP", frame->ebp, 8);
    line_append_hex(&line, "ESP", esp, 8);
    line_show(&line, VGA_WHITE_RED, PANEL_ROW + 3);

    line_append_hex(&line, "DS", frame->ds, 4);
    line_append_hex(&line, "ES", frame->es, 4);
    line_append_hex(&line, "FS", frame->fs, 4);
    line_append_hex(&line, "GS", frame->gs, 4);
    line_append_hex(&line, "CR2", cr2, 8);
    line_append_hex(&line, "CR3", cr3, 8);
    line_show(&line, VGA_WHITE_RED, PANEL_ROW + 4);

    for (;;)
        __asm__ volatile("cli; hlt");
}

/*
 * 这个异常是不是当前指令造成的。NMI（2）、双重错误（8）、机器检查（18）来自硬件，
 * 或者表示内核处理异常时又出了错，与用户程序无关：即使发生时 CPU 恰好在用户态，也不能算到它头上。
 */
static int caused_by_program(uint32_t vector)
{
    return vector != 2 && vector != 8 && vector != 18;
}

/*
 * 用户态程序出错：只结束出错的任务，内核和其他任务照常运行。
 * 在第 7 行写一条消息，说明哪个任务因为什么异常、在哪条指令被结束。
 */
static void user_exception(const struct interrupt_frame *frame)
{
    struct text_line line;
    const char *name = frame->vector < 20 ? exception_names[frame->vector] : "reserved";

    line_init(&line);
    line_append(&line, "task ");
    line_append_dec(&line, (uint32_t)current_pid());
    line_append(&line, " killed by ");
    line_append(&line, name);
    line_append(&line, ": ");
    line_append_hex(&line, "EIP", frame->eip, 8);
    line_append_hex(&line, "error", frame->error_code, 8);
    line_show(&line, VGA_YELLOW, TASK_MESSAGE_ROW);
    sched_exit_current();
}

void interrupt_dispatch(struct interrupt_frame *frame)
{
    uint32_t vector = frame->vector;

    if (vector < IRQ_BASE) {
        if (from_user_mode(frame) && caused_by_program(vector))
            user_exception(frame);  /* 不会返回 */
        exception_panic(frame);     /* 内核自己出错，或者硬件出了问题，无法恢复 */
        return;
    }
    if (vector == SYSCALL_VECTOR) {
        syscall_dispatch(frame);    /* 不是 8259 送来的，不能发 EOI */
        return;
    }

    /*
     * 先发 EOI 再处理：定时器处理函数可能切换任务，而新任务第一次运行时从任务入口开始，
     * 不会回到这里。若 EOI 留到处理完才发，8259 就再也不会送来定时器中断。
     * 中断门已清除 IF，提前发 EOI 也不会让中断嵌套进来。Linux 0.11 的 timer_interrupt 同样先发 EOI。
     */
    pic_send_eoi(vector);

    if (vector == IRQ_TIMER)
        timer_handle();
    else if (vector == IRQ_KEYBOARD)
        keyboard_handle();
}
