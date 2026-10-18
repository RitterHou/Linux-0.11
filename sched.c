/*
 * 任务调度：任务表、硬件任务切换、睡眠与唤醒、结束任务。
 *
 * 用 x86 的硬件任务切换，与 Linux 0.11 相同：每个任务有一个 TSS（任务状态段），
 * GDT 中有它的描述符。远跳转到 TSS 的选择子时，CPU 自动把当前的通用寄存器、段寄存器、
 * EFLAGS 和 EIP 存进当前任务的 TSS，再从目标任务的 TSS 装入这些寄存器以及 CR3，
 * 于是目标任务从它上次被切走的地方接着运行。CR3 只装入、不保存，每个 TSS 都要预先填好。
 *
 * 任务表有 NR_TASKS 个槽，槽号就是任务号（pid）：
 *   - 任务 0 是 kmain() 自己：sched_init() 用 ltr 把它登记为当前任务，调度开始后它当空闲任务，
 *     只在别的任务都不能运行时才运行。
 *   - 任务 1（A）在内核态（特权级 0）运行 task_main()；任务 2（B）在用户态（特权级 3）运行
 *     user.c 的 user_main()。两者做同样的事，都只通过系统调用输出和等待。
 * 除任务 0 外，每个任务的内核栈是从 mem.c 分来的一页；用户任务还有自己的页目录（见 paging.c），
 * 内核部分与别的任务共用，用户栈区各自独立。Linux 0.11 也是每个进程一页内核栈，放在 task_struct 的同一页里。
 *
 * 什么时候切换：
 *   - 任务调用 sleep：立刻把 CPU 让给别的任务，没有就让给任务 0（sched_sleep）。
 *   - 定时器中断：先叫醒到时间的任务；任务 0 在运行就马上让给它，否则当前任务用完时间片才切换（schedule）。
 * 第二条是抢占：一直不睡的任务也会被切走，别的任务照样有机会运行。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"
#include "selftest.h"
#include "syscall.h"

#define TSS_ACCESS     0x89         /* 存在、DPL 0、类型 9：32 位可用 TSS */
#define TASK_SLICE     20           /* 不睡的任务最多连续运行 20 个 tick（200 ms）就被切走 */

#define TASK_UNUSED    0            /* 空槽，或者任务已经结束 */
#define TASK_RUNNING   1            /* 能运行：正在运行，或者等着轮到它 */
#define TASK_SLEEPING  2            /* 在 sleep 里等时间到，不参与调度 */
#define TASK_ZOMBIE    3            /* 已经结束，内存已收回，等父进程用 waitpid 取走退出码 */
#define TASK_WAITING   4            /* 在 waitpid 里等子进程结束，不参与调度 */
#define TASK_BLOCKED   5            /* 睡在某个等待队列上（例如等硬盘），等 wake_up 叫醒 */

#define INIT_TASK      2            /* init：内核建的用户任务，收养孤儿、拉起 shell，不能被信号结束 */

#define OUTPUT_ROW     6
#define OUTPUT_COL     7            /* 紧跟在 "tasks: " 后面 */
#define OUTPUT_WIDTH   (80 - OUTPUT_COL)

#define IDLE_ROW       4            /* 空闲比例写在 "ticks: N" 的右边 */
#define IDLE_COL       20

/* 32 位 TSS 的格式由 CPU 规定，共 104 字节。 */
struct tss {
    uint32_t prev_task;             /* 用 call 切换任务时的返回链，这里不用 */
    uint32_t esp0, ss0, esp1, ss1, esp2, ss2;   /* 从用户态进入特权级 0 时用 ss0:esp0 */
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

/* 任务表中 CPU 不关心的部分。TSS 单独放一个数组，GDT 的描述符指向它们。 */
struct task {
    int state;
    uint32_t wake_at;               /* 睡眠的任务在 timer_ticks 到达这个值时被叫醒 */
    uint32_t page_dir;              /* 页目录的物理地址，也就是 TSS 里的 CR3 */
    uint32_t kstack;                /* 内核栈所在的页；任务 0 用 entry.asm 的栈，这里是 0 */
    int parent;                     /* 父进程的任务号；内核建的任务和孤儿是 0，没有人等它们 */
    uint32_t status;                /* 结束时的状态，waitpid 取走：exit 的退出码、0x100 加异常号或 0x200 加信号编号 */
    struct file *files[NR_OPEN];    /* 文件描述符表：第 fd 项指向打开的文件，0 表示没用 */
    char name[TASK_NAME_LEN];       /* ps 显示的名字：内核建的任务起好的名字，exec 后是程序的文件名 */
    char cwd[PATH_MAX];             /* 当前目录：规范化过的绝对路径，内核建的任务是 /，fork 时继承 */
    uint32_t signal;                /* 收到、还没处理的信号：第 sig - 1 位 */
    uint32_t action[32];            /* 每个信号来了怎么办：SIG_DFL、SIG_IGN 或用户的处理函数；fork 时继承 */
    uint32_t restorer;              /* 处理函数返回到这里（用户程序里的 sig_restore） */
    int pgrp;                       /* 进程组：Ctrl+C 发给终端的前台进程组里的每个任务；fork 时继承 */
    int leader;                     /* 调用过 setsid，是进程组的头：它打开终端时，终端的前台进程组就是它这一组 */
    int has_tty;                    /* 进程组的头已经有了控制终端（打开过 /dev/tty0）；结束时把终端的前台进程组清掉 */
    int interruptible;              /* 睡在 interruptible_sleep_on 里：信号能叫醒它 */
};

static struct tss tss[NR_TASKS];
static struct task tasks[NR_TASKS];

static int current_task;            /* 当前任务号，起始为 0 */
static int scheduling;              /* sched_start() 之后才开始切换 */
static uint32_t slice_left;         /* 当前任务还剩几个 tick 的时间片 */
static int output_pos;              /* 各任务共用的输出位置 */

/*
 * 写一个字符到第 6 行，供 write 系统调用使用。各任务共用 output_pos，
 * 关中断保证“读位置、写字符、位置加一”不被任务切换打断；结束后恢复原来的中断状态。
 * 第 6 行只有一行，换行没有意义，直接忽略。
 */
void task_write_char(char ch)
{
    char s[2] = { ch, '\0' };
    if (ch == '\n')
        return;
    uint32_t flags = irq_save();

    vga_write_at(OUTPUT_ROW, OUTPUT_COL + output_pos, s, VGA_GRAY);
    output_pos = (output_pos + 1) % OUTPUT_WIDTH;
    irq_restore(flags);
}

int current_pid(void)
{
    return current_task;
}

/* 当前任务的文件描述符表。 */
struct file **current_files(void)
{
    return tasks[current_task].files;
}

/* 当前任务的当前目录。 */
char *current_cwd(void)
{
    return tasks[current_task].cwd;
}

static void set_name(int n, const char *name)
{
    int i = 0;
    for (; i < TASK_NAME_LEN - 1 && name[i]; i++)
        tasks[n].name[i] = name[i];
    tasks[n].name[i] = '\0';
}

/* exec 成功后用程序的文件名（路径最后一段）当任务的名字。 */
void sched_set_name(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            base = p + 1;
    set_name(current_task, base);
}

/* taskinfo 系统调用：任务 pid 的状态、父进程和名字。空槽返回 -1。 */
int sched_taskinfo(int pid, struct taskinfo *info)
{
    if (pid < 0 || pid >= NR_TASKS)
        return -1;
    uint32_t flags = irq_save();
    struct task t = tasks[pid];
    irq_restore(flags);
    if (t.state == TASK_UNUSED)
        return -1;
    info->state = t.state;
    info->parent = t.parent;
    for (int i = 0; i < TASK_NAME_LEN; i++)
        info->name[i] = t.name[i];
    return 0;
}

/* 内核建的任务开始时只有 1 号描述符：第 6 行的任务输出区。 */
static void give_tasks_row(int n)
{
    struct file *f = tasks_file();
    file_get(f);
    tasks[n].files[1] = f;
}

/*
 * 任务 A 的代码（特权级 0）。任务 B 的 user_main() 与它做同样的事，只是运行在用户态：
 * 用 getpid() 得到任务号，任务 1 写 A、任务 2 写 B；每写一个字母 sleep 2 个 tick。
 */
static void task_main(void)
{
    selftest_task_a();              /* 正常构建为空；自检 HD_RW 在这里读写硬盘 */
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

/*
 * 占用任务 n 的槽：分一页当内核栈，TSS 先填共同的部分。没有空闲页时返回 0。
 * 新任务的 EFLAGS 是 0x202：IF=1，一开始运行就允许中断；第 1 位规定为 1。
 */
static int setup_task(int n, void (*entry)(void), uint32_t cr3)
{
    uint32_t kstack = get_free_page();

    if (kstack == 0)
        return 0;
    tasks[n] = (struct task){ .state = TASK_RUNNING, .page_dir = cr3, .kstack = kstack, .cwd = "/", .pgrp = n };
    tss[n] = (struct tss){ .eip = (uint32_t)entry, .eflags = 0x202, .cr3 = cr3,
                           .iomap_base = sizeof(struct tss) };   /* 没有 I/O 权限位图 */
    give_tasks_row(n);
    return 1;
}

/* 内核任务：特权级 0，栈就是它的内核栈，用内核的页目录。 */
static void init_task(int n, void (*entry)(void))
{
    if (!setup_task(n, entry, kernel_page_dir()))
        panic("no memory for a kernel task");
    struct tss *t = &tss[n];
    t->esp = tasks[n].kstack + PAGE_SIZE;
    t->cs = KERNEL_CODE_SEL;
    t->ds = t->es = t->fs = t->gs = t->ss = KERNEL_DATA_SEL;
}

/*
 * 用户任务：有自己的页目录；CS 等填用户段选择子（RPL 为 3），硬件任务切换装入这个 TSS 时就直接进入特权级 3。
 * ss0:esp0 是它的内核栈：在用户态发生中断或系统调用时，CPU 从这里取出内核栈，
 * 再把用户态的 SS、ESP、EFLAGS、CS、EIP 压上去。EFLAGS 的 IOPL 为 0，用户态不能执行 cli、in、out。
 * Linux 0.11 的 copy_process() 也把 esp0 设为该进程内核栈的顶端。
 */
static void init_user_task(int n, void (*entry)(void), uint32_t user_esp)
{
    uint32_t dir = new_page_dir();

    if (dir == 0 || !setup_task(n, entry, dir))
        panic("no memory for a user task");
    struct tss *t = &tss[n];
    t->esp = user_esp;
    t->cs = USER_CODE_SEL;
    t->ds = t->es = t->fs = t->gs = t->ss = USER_DATA_SEL;
    t->ss0 = KERNEL_DATA_SEL;
    t->esp0 = tasks[n].kstack + PAGE_SIZE;
}

void sched_init(void)
{
    /*
     * 任务 0 的 TSS 也要填 CR3：任务切换时 CPU 不会把当前的 CR3 存进旧 TSS，
     * 切回任务 0 时装入的就是这里的值，留 0 会把物理地址 0 当成页目录。
     * Linux 0.11 的 INIT_TASK 同样把 cr3 填为页目录 pg_dir。
     */
    tss[0].cr3 = kernel_page_dir();
    tss[0].iomap_base = sizeof(struct tss);
    tasks[0] = (struct task){ .state = TASK_RUNNING, .page_dir = kernel_page_dir(), .cwd = "/" };
    give_tasks_row(0);              /* 自检会在开始调度前从 kmain 调用 write */
    set_name(0, "idle");
    for (int n = 0; n < NR_TASKS; n++)
        gdt_set_entry(TSS_GDT_INDEX + n, (uint32_t)&tss[n], sizeof(struct tss) - 1, TSS_ACCESS, 0);

    init_task(1, task_main);
    set_name(1, "task_a");
    init_user_task(2, user_main, USER_TOP);   /* 栈顶的页第一次用到时才分配 */
    set_name(2, "init");

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
 * 从当前任务的下一个槽起，按 1、2、…、NR_TASKS - 1 的顺序转一圈，找第一个能运行的任务
 * （可能就是当前任务自己）。一个都没有时返回 0：任务 0 是 kmain 的 hlt 循环，正好当空闲任务。
 */
static int pick_next(void)
{
    int n = current_task;

    for (int k = 0; k < NR_TASKS - 1; k++) {
        n = n % (NR_TASKS - 1) + 1;
        if (tasks[n].state == TASK_RUNNING)
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
    for (int n = 1; n < NR_TASKS; n++)
        if (tasks[n].state == TASK_SLEEPING && (int32_t)(timer_ticks - tasks[n].wake_at) >= 0)
            tasks[n].state = TASK_RUNNING;
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
 * 外设中断处理完时调用：此刻要是空闲的任务 0 在运行，而中断处理函数叫醒了某个任务，就马上切过去，
 * 不必等下一个定时器中断。任务 0 停在 hlt 上，只有中断能让它往下走。
 */
void sched_idle_wakeup(void)
{
    if (scheduling && current_task == 0)
        reschedule();
}

/*
 * 在等待队列 q 上睡下，直到有人 wake_up(q)。调用者关着中断，醒来时中断仍是关着的：
 * 典型的用法是 while (条件不满足) sleep_on(&q);，醒来后要重新检查条件，可能有别的任务抢先了。
 * 任务 0 不能睡：它是空闲任务，没有它，没事做的时候 CPU 无处可去。
 * Linux 0.11 的 sleep_on() 用每个睡眠者栈上的 tmp 变量把它们串成一条链；任务最多 16 个，这里用一个位图。
 */
void sleep_on(struct wait_queue *q)
{
    if (!scheduling || current_task == 0)
        panic("task 0 trying to sleep");
    q->sleepers |= 1u << current_task;
    tasks[current_task].state = TASK_BLOCKED;
    reschedule();
}

/* 叫醒睡在 q 上的所有任务。它们醒来后各自重新检查条件。 */
void wake_up(struct wait_queue *q)
{
    for (int n = 1; n < NR_TASKS; n++)
        if ((q->sleepers & (1u << n)) && tasks[n].state == TASK_BLOCKED)
            tasks[n].state = TASK_RUNNING;
    q->sleepers = 0;
}

/*
 * 和 sleep_on 一样睡下，但信号也能叫醒它：终端、管道这些不知道要等多久的地方用它。
 * 返回 -1 表示有信号（睡之前就有，或者是被信号叫醒的），调用者应该放弃这次系统调用，返回 -EINTR；
 * 回到用户态之前，interrupt_dispatch() 再按信号的处理方式处理：结束进程，或者先去调用处理函数。等硬盘、等缓冲区、等文件系统锁
 * 很快就有结果，半途放弃还会留下改到一半的数据，所以仍用 sleep_on，信号要等它们做完才处理。
 * Linux 0.11 的 interruptible_sleep_on() 把状态设为 TASK_INTERRUPTIBLE，schedule() 看到它有信号就改回能运行。
 */
int interruptible_sleep_on(struct wait_queue *q)
{
    struct task *t = &tasks[current_task];

    if (t->signal)
        return -1;
    t->interruptible = 1;
    sleep_on(q);
    t->interruptible = 0;
    q->sleepers &= ~(1u << current_task);   /* 被信号叫醒的话，wake_up 没来清这一位：不清的话，“位在队列里”就不再意味着“睡在上面” */
    return t->signal ? -1 : 0;
}

/*
 * 给任务 n 挂上信号 sig。它要是睡在能打断的地方（sleep、waitpid、interruptible_sleep_on），就改回能运行，
 * 让它从系统调用里出来，回到用户态之前处理这个信号。调用者关着中断。
 * 它忽略的信号直接丢掉，也不叫醒它：后台程序忽略 SIGINT，按 Ctrl+C 不会打断它的 sleep。
 * SIGKILL 总能送到：sched_signal() 不让任何进程忽略它。
 * Linux 0.11 的 send_sig() 只挂上信号，由 schedule() 叫醒 TASK_INTERRUPTIBLE 的任务，忽略要等处理时才看。
 */
static void send_signal(int n, int sig)
{
    struct task *t = &tasks[n];
    uint32_t bit = 1u << (sig - 1);

    if (t->action[sig - 1] == (uint32_t)SIG_IGN)
        return;
    t->signal |= bit;
    if (t->state == TASK_SLEEPING || t->state == TASK_WAITING || (t->state == TASK_BLOCKED && t->interruptible))
        t->state = TASK_RUNNING;
}

/*
 * kill 系统调用：pid > 0 发给那个任务；pid 为 0 发给自己所在的进程组（自己也收到）；pid < -1 发给 -pid 那一组。
 * 任务 0、A 和 init 收不到：前两个是内核任务，从不回到用户态，信号永远处理不了；init 结束了就没人收养孤儿、拉起 shell。
 * 一个也没找到返回 -1。Linux 0.11 的 sys_kill() 一样按 pid 分几种情况；pid 为 -1 时发给所有进程，这里不支持。
 */
int sched_kill(int pid, int sig)
{
    if (sig < 1 || sig > 32 || pid == -1)
        return -1;
    uint32_t flags = irq_save();
    int group = pid == 0 ? tasks[current_task].pgrp : -pid, found = 0;

    for (int n = INIT_TASK + 1; n < NR_TASKS; n++) {
        if (tasks[n].state == TASK_UNUSED || (pid > 0 ? n != pid : tasks[n].pgrp != group))
            continue;
        found = 1;
        if (tasks[n].state != TASK_ZOMBIE)  /* 已经结束了，信号也没用了 */
            send_signal(n, sig);
    }
    irq_restore(flags);
    return found ? 0 : -1;
}

/*
 * signal 系统调用：这个信号来了怎么办：SIG_DFL 结束进程，SIG_IGN 不理，否则是用户的处理函数，
 * restorer 是处理函数返回的地方（见 interrupt.c 的 handle_signals()）。返回原来的处理方式。
 * 改成忽略时，已经挂着的这个信号也丢掉。SIGKILL 改不了：总得有办法结束一个进程。
 * Linux 0.11 的 sys_signal() 设的是一次性的处理函数（SA_ONESHOT）：处理一次就改回默认，要用的话
 * 得在处理函数里再设一遍；这里像后来的 Unix 一样一直有效。
 */
int sched_signal(int sig, uint32_t handler, uint32_t restorer)
{
    if (sig < 1 || sig > 32 || sig == SIGKILL)
        return -1;
    if (handler > (uint32_t)SIG_IGN && (!user_range_ok(handler, 1) || !user_range_ok(restorer, 1)))
        return -1;                      /* 处理函数在用户态运行，必须是用户自己的内存 */
    struct task *t = &tasks[current_task];
    uint32_t flags = irq_save();        /* 键盘中断随时可能往 t->signal 里挂信号 */
    uint32_t old = t->action[sig - 1];

    t->action[sig - 1] = handler;
    if (handler == (uint32_t)SIG_IGN)
        t->signal &= ~(1u << (sig - 1));
    else if (handler != (uint32_t)SIG_DFL)
        t->restorer = restorer;
    irq_restore(flags);
    return (int)old;
}

/*
 * exec 换了程序，原来的处理函数在新程序里什么也不是：设过处理函数的信号都改回默认，忽略的保持忽略（POSIX 的规定）。
 * Linux 0.11 的 do_execve() 把 32 个都改回默认，连忽略的也改回来。
 */
void sched_exec_signals(void)
{
    struct task *t = &tasks[current_task];
    for (int i = 0; i < 32; i++)
        if (t->action[i] != (uint32_t)SIG_IGN)
            t->action[i] = (uint32_t)SIG_DFL;
}

/*
 * setsid 系统调用：自己当一个新的进程组的头，组号就是自己的任务号。已经当过了返回 -1。
 * init 拉起 shell 时在子进程里先调用它，再打开 /dev/tty0：终端的前台进程组就成了 shell 这一组（见 file.c）。
 * Linux 0.11 的 sys_setsid() 同时新建一个会话、断开原来的控制终端；这里没有会话，只有进程组。
 */
int sched_setsid(void)
{
    struct task *t = &tasks[current_task];

    if (t->leader)
        return -1;
    t->leader = 1;
    t->pgrp = current_task;
    return current_task;
}

/* 给当前任务自己发信号：往没人读的管道里写时发 SIGPIPE。init 和内核任务照样收不到。 */
void signal_current(int sig)
{
    uint32_t flags = irq_save();
    if (current_task > INIT_TASK)
        send_signal(current_task, sig);
    irq_restore(flags);
}

/*
 * 打开控制台时调用：进程组的头第一次打开它，就成了它的控制终端，返回 1，调用者把终端的前台进程组设成这一组。
 * 已经有了控制终端的再打开不算，不会把前台进程组抢过来。Linux 0.11 的 sys_open() 也要求 current->tty < 0。
 */
int sched_claim_tty(void)
{
    struct task *t = &tasks[current_task];

    if (!t->leader || t->has_tty)
        return 0;
    t->has_tty = 1;
    return 1;
}

int sched_pgrp(void)
{
    return tasks[current_task].pgrp;
}

int sched_is_leader(void)
{
    return tasks[current_task].leader;
}

/*
 * 取出一个要处理的信号：编号最小的那个，连同它的处理方式和 restorer；没有返回 0。
 * 回到用户态之前由 interrupt_dispatch() 调用。
 */
int sched_take_signal(uint32_t *handler, uint32_t *restorer)
{
    struct task *t = &tasks[current_task];
    uint32_t flags = irq_save();
    int sig = 0;

    if (t->signal) {
        while (!(t->signal & (1u << sig)))
            sig++;
        t->signal &= ~(1u << sig);
        *handler = t->action[sig];
        *restorer = t->restorer;
        sig++;
    }
    irq_restore(flags);
    return sig;
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
    if (ticks > 0x7FFFFFFF)         /* wake_sleepers() 用有符号的差值比较，再长就会被当成已经到时间 */
        ticks = 0x7FFFFFFF;
    if (!scheduling || current_task == 0) {
        /* 任务 0 没有可以让给的任务，只能原地等。自检会在开始调度前从 kmain 调用 sleep。 */
        while (timer_ticks - start < ticks)
            __asm__ volatile("sti; hlt");
        return;
    }
    uint32_t flags = irq_save();
    if (!tasks[current_task].signal) {  /* 有信号就不睡了；睡着的时候来了信号，send_signal() 也会提前叫醒它 */
        tasks[current_task].state = TASK_SLEEPING;
        tasks[current_task].wake_at = start + ticks;
        reschedule();
    }
    irq_restore(flags);
}

/*
 * fork 系统调用：复制当前的用户任务，返回新任务号；任务槽或内存不够时返回 -1。
 * 新任务的 TSS 直接用父进程进入系统调用时 CPU 和 isr_common 压在栈上的寄存器：
 * EIP 是 int 0x80 的下一条指令，CS、SS、ESP 是用户态的，只把 EAX 改成 0。
 * 于是新任务第一次被调度时，硬件任务切换直接把它放回用户态，好像 fork 刚返回 0。
 * 它的内核栈是新的一页，此刻是空的：它从没在内核里运行过。
 * Linux 0.11 的 copy_process() 也是用栈上的寄存器填新进程的 TSS，再把 eax 设为 0。
 * 整个过程关着中断：找空槽和占用它之间不能被别的任务插进来抢走同一个槽。
 */
int sched_fork(const struct interrupt_frame *frame)
{
    uint32_t flags = irq_save();
    int n = 1;

    while (n < NR_TASKS && (tasks[n].state != TASK_UNUSED || tasks[n].kstack != 0))
        n++;
    uint32_t kstack = n < NR_TASKS ? get_free_page() : 0;
    uint32_t dir = kstack ? copy_page_dir(tasks[current_task].page_dir) : 0;
    if (dir == 0) {
        if (kstack)
            free_page(kstack);
        irq_restore(flags);
        return -1;
    }
    tss[n] = (struct tss){
        .esp0 = kstack + PAGE_SIZE, .ss0 = KERNEL_DATA_SEL, .cr3 = dir,
        .eip = frame->eip, .eflags = frame->eflags,
        .eax = 0, .ecx = frame->ecx, .edx = frame->edx, .ebx = frame->ebx,
        .esp = frame->user_esp, .ebp = frame->ebp, .esi = frame->esi, .edi = frame->edi,
        .es = frame->es, .cs = frame->cs, .ss = frame->user_ss, .ds = frame->ds, .fs = frame->fs, .gs = frame->gs,
        .iomap_base = sizeof(struct tss),
    };
    tasks[n] = (struct task){ .state = TASK_RUNNING, .page_dir = dir, .kstack = kstack, .parent = current_task,
                              .restorer = tasks[current_task].restorer, .pgrp = tasks[current_task].pgrp };   /* 收到的信号不继承 */
    for (int i = 0; i < 32; i++)
        tasks[n].action[i] = tasks[current_task].action[i];
    set_name(n, tasks[current_task].name);
    for (int i = 0; i < PATH_MAX; i++)
        tasks[n].cwd[i] = tasks[current_task].cwd[i];
    for (int fd = 0; fd < NR_OPEN; fd++) {      /* 子进程继承所有打开的文件，与父进程共用读写位置 */
        tasks[n].files[fd] = tasks[current_task].files[fd];
        if (tasks[n].files[fd])
            file_get(tasks[n].files[fd]);
    }
    irq_restore(flags);
    return n;
}

/*
 * exec 换好了新的页目录：记进任务表和 TSS（下次切回来时装入），并马上装入 CR3。返回旧的页目录，由调用者收回。
 * 中断关着做：要是记进任务表之后、装入 CR3 之前被切走，切回来时会装入新的，也没错，但这样更清楚。
 */
uint32_t sched_set_page_dir(uint32_t dir)
{
    uint32_t flags = irq_save();
    uint32_t old = tasks[current_task].page_dir;
    tasks[current_task].page_dir = dir;
    tss[current_task].cr3 = dir;
    __asm__ volatile("movl %0, %%cr3" : : "r"(dir) : "memory");
    irq_restore(flags);
    return old;
}

/* 收走一个已经结束的任务：还掉内核栈，任务槽空出来。 */
static void release(int n)
{
    free_page(tasks[n].kstack);
    tasks[n] = (struct task){ .state = TASK_UNUSED };
}

/*
 * waitpid 系统调用：等子进程结束，取走它的状态，返回它的任务号。pid 为 -1 时等任意一个子进程。
 * 子进程还在运行，就把自己标成 TASK_WAITING 切走，子进程结束时把父进程叫醒，再重新找一遍；
 * 带 WNOHANG 时不等，返回 0。没有符合条件的子进程返回 -1。
 * 查找和睡下之间关着中断，子进程不会恰好在这中间结束，叫醒就不会落空。
 * status 写在最后、开着中断：它可能落在 fork 留下的只读页上，写的时候要走一次写时复制。
 * Linux 0.11 的 sys_waitpid() 也是这样一圈圈找，找到僵尸就 release() 它，找不到就睡下再来。
 */
int sched_waitpid(int pid, uint32_t *status, int options)
{
    for (;;) {
        uint32_t flags = irq_save();
        int have_child = 0;

        for (int n = 1; n < NR_TASKS; n++) {
            if (tasks[n].state == TASK_UNUSED || tasks[n].parent != current_task)
                continue;
            if (pid > 0 && n != pid)
                continue;
            have_child = 1;
            if (tasks[n].state == TASK_ZOMBIE) {
                uint32_t st = tasks[n].status;
                release(n);
                irq_restore(flags);
                if (status)
                    *status = st;
                return n;
            }
        }
        if (!have_child || (options & WNOHANG)) {
            irq_restore(flags);
            return have_child ? 0 : -1;
        }
        if (tasks[current_task].signal) {      /* 等着的时候来了信号：send_signal() 把它叫醒了 */
            irq_restore(flags);
            return -EINTR;
        }
        tasks[current_task].state = TASK_WAITING;
        reschedule();
        irq_restore(flags);
    }
}

/*
 * 结束当前任务，切到下一个任务，不再返回。用户态程序出错或调用 exit 时由内核调用，
 * status 是留给父进程的状态。
 *   - 用户内存当场收回：用户栈区的页、页表和页目录。
 *   - 自己的子进程成了孤儿，交给任务 0：没有人会再等它们，已经结束的当场收走，以后结束的也直接收走。
 *   - 有父进程等着的，变成僵尸：内核栈和任务槽留着，父进程 waitpid 时取走状态再释放；父进程正在
 *     waitpid 里睡着，就叫醒它。没有父进程的（内核建的任务、孤儿），内核栈也当场还掉。
 * 此刻还在用这个页目录和内核栈，但中断关着，切走之前没有人会分配内存，这几页的内容不会被改掉；
 * 切换时 CPU 把寄存器存进静态的 tss 数组，也不碰它们。
 * Linux 0.11 的 do_exit() 也是先 free_page_tables()，再把子进程交给 init（任务 1），自己变成
 * TASK_ZOMBIE，用 SIGCHLD 通知父进程；内核栈所在的那一页由父进程的 waitpid 调用 release() 释放。
 */
void sched_exit(uint32_t status)
{
    struct task *t = &tasks[current_task];

    /*
     * 先关闭所有打开的文件：最后一个关掉已经删了的文件时要收回它的块，要读写硬盘、会睡，
     * 所以必须趁内核栈和页目录都还在的时候做。
     */
    for (int fd = 0; fd < NR_OPEN; fd++) {
        struct file *f = t->files[fd];
        t->files[fd] = 0;
        if (f)
            file_put(f);
    }
    if (t->leader && t->has_tty)
        tty_release_pgrp(t->pgrp);  /* 终端的前台进程组没了头：Ctrl+C 不再发给这一组。Linux 0.11 的 do_exit() 也这样 */
    __asm__ volatile("cli");        /* 不再返回，切到的任务从自己的 TSS 装入 EFLAGS，中断状态随之恢复 */
    if (t->page_dir != kernel_page_dir())
        free_page_dir(t->page_dir);
    t->page_dir = 0;
    for (int n = 1; n < NR_TASKS; n++) {
        if (tasks[n].state != TASK_UNUSED && tasks[n].parent == current_task) {
            tasks[n].parent = 0;
            if (tasks[n].state == TASK_ZOMBIE)
                release(n);
        }
    }
    t->status = status;
    if (t->parent == 0) {
        free_page(t->kstack);
        *t = (struct task){ .state = TASK_UNUSED };
    } else {
        t->state = TASK_ZOMBIE;
        if (tasks[t->parent].state == TASK_WAITING)
            tasks[t->parent].state = TASK_RUNNING;
    }
    reschedule();
    for (;;)                        /* 不会执行到这里 */
        __asm__ volatile("hlt");
}
