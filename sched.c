/*
 * 任务调度：任务 A 和任务 B 在定时器中断的驱动下轮流运行，各自把字母写到第 6 行。
 *
 * 用 x86 的硬件任务切换，与 Linux 0.11 相同：每个任务有一个 TSS（任务状态段），
 * GDT 中有它的描述符。远跳转到 TSS 的选择子时，CPU 自动把当前的通用寄存器、段寄存器、
 * EFLAGS 和 EIP 存进当前任务的 TSS，再从目标任务的 TSS 装入这些寄存器以及 CR3，
 * 于是目标任务从它上次被切走的地方接着运行。CR3 只装入、不保存，每个 TSS 都要预先填好。
 *
 * 任务 0 是 kmain() 自己：sched_init() 用 ltr 把它登记为当前任务，调度开始后它当空闲任务，
 * 只在 A、B 都不能运行时才运行。
 * 任务 A 在内核态（特权级 0）运行 task_main()；任务 B 在用户态（特权级 3）运行 user.c 的 user_main()，
 * 两者做同样的事，都只通过系统调用输出和等待。
 *
 * 什么时候切换：
 *   - 任务调用 sleep：立刻把 CPU 让给别的任务，没有就让给任务 0（sched_sleep）。
 *   - 定时器中断：先叫醒到时间的任务；任务 0 在运行就马上让给它，否则当前任务用完时间片才切换（schedule）。
 * 第二条是抢占：一直不睡的任务也会被切走，别的任务照样有机会运行。
 */
#include "io.h"
#include "kernel.h"
#include "selftest.h"
#include "syscall.h"

#define TASKS          3            /* 0 = kmain，1 = A，2 = B */
#define USER_TASK      2            /* 任务 B 在用户态运行，用户栈区归它 */
#define TSS_GDT_INDEX  3            /* 任务 n 的 TSS 描述符在 GDT 第 3 + n 项 */
#define TSS_SEL(n)     ((TSS_GDT_INDEX + (n)) * 8)
#define TSS_ACCESS     0x89         /* 存在、DPL 0、类型 9：32 位可用 TSS */
#define TASK_SLICE     20           /* 不睡的任务最多连续运行 20 个 tick（200 ms）就被切走 */

#define TASK_UNUSED    0            /* 没有任务，或者任务已经结束 */
#define TASK_RUNNING   1            /* 能运行：正在运行，或者等着轮到它 */
#define TASK_SLEEPING  2            /* 在 sleep 里等时间到，不参与调度 */
#define STACK_SIZE     4096

#define OUTPUT_ROW     6
#define OUTPUT_COL     7            /* 紧跟在 "tasks: " 后面 */
#define OUTPUT_WIDTH   (80 - OUTPUT_COL)

#define IDLE_ROW       4            /* 空闲比例写在 "ticks: N" 的右边 */
#define IDLE_COL       20

/* 32 位 TSS 的格式由 CPU 规定，共 104 字节。 */
struct tss {
    uint32_t prev_task;             /* 用 call 切换任务时的返回链，这里不用 */
    uint32_t esp0, ss0, esp1, ss1, esp2, ss2;   /* 特权级变化时用的栈，这里都在 0 级 */
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct tss tss[TASKS];
/*
 * 任务 1、2 各自的内核栈；任务 0 就是 kmain，沿用 entry.asm 设置的内核栈。
 * 任务 B 在用户态时用 user.c 中的用户栈，进入中断或系统调用时 CPU 换到这里的内核栈。
 */
static uint8_t task_stacks[TASKS - 1][STACK_SIZE] __attribute__((aligned(16)));

static int current_task;            /* 当前任务号，起始为 0 */
/* 任务 1、2 的状态。任务 0 是空闲任务，不睡也不结束，它的状态不用；Linux 0.11 的 task[0] 也是这样。 */
static int task_state[TASKS];
static uint32_t wake_at[TASKS];     /* 睡眠的任务在 timer_ticks 到达这个值时被叫醒 */
static int scheduling;              /* sched_start() 之后才开始切换 */
static uint32_t slice_left;         /* 当前任务还剩几个 tick 的时间片 */
static int output_pos;              /* 两个任务共用的输出位置 */

/*
 * 写一个字符到第 6 行，供 write 系统调用使用。各任务共用 output_pos，
 * 关中断保证“读位置、写字符、位置加一”不被任务切换打断；结束后恢复原来的中断状态。
 */
void task_write_char(char ch)
{
    char s[2] = { ch, '\0' };
    uint32_t flags = irq_save();

    vga_write_at(OUTPUT_ROW, OUTPUT_COL + output_pos, s, VGA_GRAY);
    output_pos = (output_pos + 1) % OUTPUT_WIDTH;
    irq_restore(flags);
}

int current_pid(void)
{
    return current_task;
}

/*
 * 任务 A 的代码（特权级 0）。任务 B 的 user_main() 与它做同样的事，只是运行在用户态：
 * 用 getpid() 得到任务号，任务 1 写 A、任务 2 写 B；每写一个字母 sleep 2 个 tick。
 */
static void task_main(void)
{
    char letter = (char)('A' + getpid() - 1);

#if defined(SELFTEST) && SELFTEST == SELFTEST_ALL_EXIT
    for (int i = 0; i < 5; i++) {   /* 自检：写 5 个字母后结束；任务 B 写完 bye 就结束了 */
        write(1, &letter, 1);
        sleep(2);
    }
    exit(1);
#endif
    for (;;) {
        write(1, &letter, 1);
        sleep(2);
    }
}

/* 内核任务：特权级 0，栈就是它的内核栈。 */
static void init_task(int n, void (*entry)(void), uint32_t cr3)
{
    struct tss *t = &tss[n];

    t->eip = (uint32_t)entry;
    t->esp = (uint32_t)&task_stacks[n - 1][STACK_SIZE];
    t->eflags = 0x202;              /* IF=1：任务一开始运行就允许中断；第 1 位规定为 1 */
    t->cs = KERNEL_CODE_SEL;
    t->ds = t->es = t->fs = t->gs = t->ss = KERNEL_DATA_SEL;
    t->cr3 = cr3;                   /* 任务切换会装入 CR3，必须填当前的页目录 */
}

/*
 * 用户任务：CS 等填用户段选择子（RPL 为 3），硬件任务切换装入这个 TSS 时就直接进入特权级 3。
 * ss0:esp0 是它的内核栈：在用户态发生中断或系统调用时，CPU 从这里取出内核栈，
 * 再把用户态的 SS、ESP、EFLAGS、CS、EIP 压上去。EFLAGS 的 IOPL 为 0，用户态不能执行 cli、in、out。
 * Linux 0.11 的 copy_process() 也把 esp0 设为该进程内核栈的顶端。
 */
static void init_user_task(int n, void (*entry)(void), uint32_t user_esp, uint32_t cr3)
{
    struct tss *t = &tss[n];

    t->eip = (uint32_t)entry;
    t->esp = user_esp;
    t->eflags = 0x202;
    t->cs = USER_CODE_SEL;
    t->ds = t->es = t->fs = t->gs = t->ss = USER_DATA_SEL;
    t->ss0 = KERNEL_DATA_SEL;
    t->esp0 = (uint32_t)&task_stacks[n - 1][STACK_SIZE];
    t->cr3 = cr3;
}

void sched_init(void)
{
    uint32_t cr3;
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));

    /*
     * 任务 0 的 TSS 也要填 CR3：任务切换时 CPU 不会把当前的 CR3 存进旧 TSS，
     * 将来切回任务 0（所有任务都结束时）装入的就是这里的值，留 0 会把物理地址 0 当成页目录。
     * Linux 0.11 的 INIT_TASK 同样把 cr3 填为页目录 pg_dir。
     */
    tss[0].cr3 = cr3;
    for (int n = 0; n < TASKS; n++) {
        tss[n].iomap_base = sizeof(struct tss);     /* 没有 I/O 权限位图 */
        gdt_set_entry(TSS_GDT_INDEX + n, (uint32_t)&tss[n], sizeof(struct tss) - 1, TSS_ACCESS, 0);
    }
    init_task(1, task_main, cr3);
    init_user_task(USER_TASK, user_main, USER_STACK_TOP, cr3);  /* 栈顶的页第一次用到时才分配 */
    task_state[1] = task_state[2] = TASK_RUNNING;

    /* 把 kmain 登记为任务 0：第一次切换时，CPU 才知道把当前寄存器存到哪个 TSS。 */
    __asm__ volatile("ltr %w0" : : "r"(TSS_SEL(0)));
}

void sched_start(void)
{
    scheduling = 1;                 /* 此刻运行的是任务 0，下一个 tick 就让给任务 A */
}

/* 远跳转到 TSS 选择子：偏移被忽略，CPU 执行任务切换。 */
static void switch_to(int next)
{
    struct {
        uint32_t offset;
        uint16_t selector;
    } __attribute__((packed)) target = { 0, (uint16_t)TSS_SEL(next) };

    current_task = next;
    __asm__ volatile("ljmp *%0" : : "m"(target) : "memory");
    /* 下次切换回本任务时，从这里继续执行。 */
}

/*
 * 按 1 → 2 → 1 … 的顺序找下一个能运行的任务（可能就是当前任务自己）。
 * 一个都没有时返回 0：任务 0 是 kmain 的 hlt 循环，正好当空闲任务。
 */
static int pick_next(void)
{
    int n = current_task;

    for (int k = 0; k < TASKS - 1; k++) {
        n = n % (TASKS - 1) + 1;
        if (task_state[n] == TASK_RUNNING)
            return n;
    }
    return 0;
}

/* 切到下一个能运行的任务，并给它一个完整的时间片。 */
static void reschedule(void)
{
    int next = pick_next();

    slice_left = TASK_SLICE;
    if (next != current_task)       /* 跳到自己的 TSS 会因为它“忙”而触发 #GP */
        switch_to(next);
}

/* 叫醒睡到时间的任务。用差值比较，timer_ticks 回绕到 0 后也对。 */
static void wake_sleepers(void)
{
    for (int n = 1; n < TASKS; n++)
        if (task_state[n] == TASK_SLEEPING && (int32_t)(timer_ticks - wake_at[n]) >= 0)
            task_state[n] = TASK_RUNNING;
}

/*
 * 每个 tick 看一眼此刻谁占着 CPU，是任务 0 就记一次空闲；每秒在第 4 行显示一次空闲比例。
 * Linux 0.11 的 do_timer() 用同样的抽样办法，给当前进程记一个 tick 的 utime 或 stime。
 */
static void account_idle(void)
{
    static uint32_t idle, total;
    char buf[12];

    if (current_task == 0)
        idle++;
    if (++total < TIMER_HZ)
        return;
    const char *digits = format_uint(idle * 100 / total, 10, buf);
    int len = 0;
    while (digits[len])
        len++;
    vga_write_at(IDLE_ROW, IDLE_COL, "idle:    %", VGA_GRAY);
    vga_write_at(IDLE_ROW, IDLE_COL + 9 - len, digits, VGA_GRAY);   /* 百分数右对齐在 % 前面 */
    idle = total = 0;
}

/* 由定时器中断调用：记空闲、叫醒到时间的任务，再决定要不要切换。 */
void schedule(void)
{
    if (!scheduling)
        return;
    account_idle();
    wake_sleepers();
    if (current_task == 0 || --slice_left == 0)
        reschedule();
}

/*
 * sleep 系统调用：当前任务睡 ticks 个 tick，立刻把 CPU 让给别的任务，没有就让给任务 0。
 * 时间到了，定时器中断里的 wake_sleepers() 把它改回能运行；轮到它时从 reschedule() 里接着往下走。
 * 关中断让“标记为睡眠”和“切走”之间插不进定时器中断；切回来时恢复原来的中断状态。
 * Linux 0.11 的 sleep_on() 也是先改 current->state，再调用 schedule()。
 */
void sched_sleep(uint32_t ticks)
{
    uint32_t start = timer_ticks;

    if (ticks == 0)
        return;
    if (!scheduling || current_task == 0) {
        /* 任务 0 没有可以让给的任务，只能原地等。自检会在开始调度前从 kmain 调用 sleep。 */
        while (timer_ticks - start < ticks)
            __asm__ volatile("sti; hlt");
        return;
    }
    uint32_t flags = irq_save();
    task_state[current_task] = TASK_SLEEPING;
    wake_at[current_task] = start + ticks;
    reschedule();
    irq_restore(flags);
}

/*
 * 结束当前任务，切到下一个任务，不再返回。用户态程序出错或调用 exit 时由内核调用。
 * 被结束的任务不会再被调度，它的 TSS 和栈就此闲置。Linux 0.11 的 do_exit() 做的事情更多：
 * 释放内存页、关闭文件、通知父进程，最后同样调用 schedule() 切走。
 */
void sched_exit_current(void)
{
    __asm__ volatile("cli");        /* 不再返回，切到的任务从自己的 TSS 装入 EFLAGS，中断状态随之恢复 */
    task_state[current_task] = TASK_UNUSED;
    if (current_task == USER_TASK)
        free_user_pages();          /* 收回它的栈：内核栈在 .bss 里，不受影响 */
    reschedule();
    for (;;)                        /* 不会执行到这里 */
        __asm__ volatile("hlt");
}
