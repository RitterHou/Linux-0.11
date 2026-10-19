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
 *   - 定时器中断：先叫醒到时间的任务；任务 0 在运行就马上让给它，否则当前任务用完这一轮的 tick 才切换（schedule）。
 * 第二条是抢占：一直不睡的任务也会被切走，别的任务照样有机会运行。
 *
 * 切给谁：与 Linux 0.11 的 schedule() 相同，每个任务有 counter（这一轮还能运行几个 tick）和 priority（每轮补多少）。
 * 能运行的任务里挑 counter 最大的；都用完了，就给每个任务补上：counter = counter / 2 + priority。
 * priority 由 nice 值定：20 - nice。nice 19 的任务一轮只有 1 个 tick，和一个 nice 0 的任务抢 CPU 时只分到 1/21。
 * 睡着的任务没用完的 counter 留一半再加上补的，醒来时排在前面：常常睡的（等键盘的 shell）反应快。
 * 每个 tick 还看一眼此刻谁在运行、在用户态还是内核态，记进它的 utime 或 stime，times 和 ps -l 读出来。
 */
#include "io.h"
#include "kernel.h"
#include "fs.h"
#include "selftest.h"
#include "syscall.h"

#define TSS_ACCESS     0x89         /* 存在、DPL 0、类型 9：32 位可用 TSS */
#define DEF_PRIORITY   20           /* nice 为 0 的任务每轮能运行 20 个 tick（200 ms）；Linux 0.11 是 15 */
#define NICE_MIN       (-20)
#define NICE_MAX       19

#define TASK_UNUSED    0            /* 空槽，或者任务已经结束 */
#define TASK_RUNNING   1            /* 能运行：正在运行，或者等着轮到它 */
#define TASK_SLEEPING  2            /* 在 sleep 里等时间到，不参与调度 */
#define TASK_ZOMBIE    3            /* 已经结束，内存已收回，等父进程用 waitpid 取走退出码 */
#define TASK_WAITING   4            /* 在 waitpid 里等子进程结束，不参与调度 */
#define TASK_BLOCKED   5            /* 睡在某个等待队列上（例如等硬盘），等 wake_up 叫醒 */
#define TASK_STOPPED   6            /* 收到 SIGSTOP、SIGTSTP 这些停下了，不参与调度，等 SIGCONT */

#define STOP_SIGNALS   ((1u << (SIGSTOP - 1)) | (1u << (SIGTSTP - 1)) | (1u << (SIGTTIN - 1)) | (1u << (SIGTTOU - 1)))

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
    uint32_t cloexec;               /* 第 fd 位为 1：exec 时关掉这个描述符（close-on-exec），fork 时继承 */
    char name[TASK_NAME_LEN];       /* ps 显示的名字：内核建的任务起好的名字，exec 后是程序的文件名 */
    char cwd[PATH_MAX];             /* 当前目录：规范化过的绝对路径，内核建的任务是 /，fork 时继承 */
    uint32_t signal;                /* 收到、还没处理的信号：第 sig - 1 位 */
    uint32_t action[32];            /* 每个信号来了怎么办：SIG_DFL、SIG_IGN 或用户的处理函数；fork 时继承 */
    uint32_t restorer;              /* 处理函数返回到这里（用户程序里的 sig_restore） */
    int pgrp;                       /* 进程组：Ctrl+C 发给终端的前台进程组里的每个任务；fork 时继承 */
    int leader;                     /* 调用过 setsid，是进程组的头：它打开终端时，终端的前台进程组就是它这一组 */
    int sid;                        /* 会话：setsid 的那个任务的号，fork 继承。一个终端属于一个会话，作业控制只在会话里面 */
    int stopsig;                    /* 停着的话，是哪个信号让它停下的 */
    int stop_reported;              /* 这次停下已经让父进程的 waitpid(WUNTRACED) 拿到过了 */
    int ctty;                       /* 进程组的头的控制终端：TTY_CONSOLE 等加一，0 是还没有；结束时把那个终端的前台进程组清掉 */
    int interruptible;              /* 睡在 interruptible_sleep_on 里：信号能叫醒它 */
    uint32_t start_brk;             /* 程序的末尾（.bss 的末尾），堆从这里开始；exec 时设好 */
    uint32_t brk;                   /* 堆的末尾：start_brk 到 brk 之间缺页时补页，再往上到栈区之间不补 */
    int alarm_on;                   /* 定了闹钟：timer_ticks 到 alarm_at 时发 SIGALRM。fork 不继承，exec 保留 */
    uint32_t alarm_at;
    int counter;                    /* 这一轮还能运行几个 tick：每个 tick 减一，到 0 就让出 CPU */
    int priority;                   /* 每一轮补多少个 tick：DEF_PRIORITY - nice，1～40。fork 继承，exec 保留 */
    uint32_t utime, stime;          /* 在用户态、内核态运行了多少个 tick */
    uint32_t cutime, cstime;        /* waitpid 收走的子进程（连同它们收走的子进程）用了多少 */
    uint16_t uid, euid, suid;       /* 实际、有效、保存的用户号：内核建的任务都是 0（root）。fork 继承 */
    uint16_t gid, egid, sgid;       /* 组号，同上 */
    struct m_inode *exec_ip;        /* 正在运行的程序文件（iget 着）：缺页时从它里面读程序的页。内核建的任务是 0 */
    struct segment segs[MAX_SEGS];  /* 程序要装入的段，exec 时从 ELF 的程序头里抄下来 */
    int nsegs;
};

static struct tss tss[NR_TASKS];
static struct task tasks[NR_TASKS];

static int current_task;            /* 当前任务号，起始为 0 */
static int scheduling;              /* sched_start() 之后才开始切换 */
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

/* 当前任务的 close-on-exec 位图。 */
uint32_t *current_cloexec(void)
{
    return &tasks[current_task].cloexec;
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
    info->uid = t.uid;
    info->nice = DEF_PRIORITY - t.priority;
    info->priority = t.priority;
    info->ticks = t.utime + t.stime;
    for (int i = 0; i < TASK_NAME_LEN; i++)
        info->name[i] = t.name[i];
    return 0;
}

/*
 * nice 系统调用：自己的 nice 值加上 inc（可以是负数），限制在 -20～19，返回新的 nice 值。
 * nice 越大越谦让：每一轮补的 tick（priority = 20 - nice）越少。这一轮剩下的 counter 不动，下一轮才按新的补。
 * Linux 0.11 的 sys_nice() 直接从 priority 里减掉 inc（减完还大于 0 才减），返回 0；
 * 返回新的 nice 值是 POSIX 的规定。只有 root 能调低 nice 值（多要 CPU），别人这样做返回 -1，什么也不改：
 * -1 也可能是新的 nice 值，所以调用者要用 nice(0) 再看一眼。Linux 0.11 不查，谁都能调高自己的优先级。
 */
int sched_nice(int inc)
{
    struct task *t = &tasks[current_task];
    if (inc < 0 && !suser())
        return -1;
    if (inc > NICE_MAX - NICE_MIN)  /* 先限制一下，加起来才不会溢出 */
        inc = NICE_MAX - NICE_MIN;
    if (inc < NICE_MIN - NICE_MAX)
        inc = NICE_MIN - NICE_MAX;
    int nice = DEF_PRIORITY - t->priority + inc;
    nice = nice < NICE_MIN ? NICE_MIN : nice > NICE_MAX ? NICE_MAX : nice;
    t->priority = DEF_PRIORITY - nice;
    return nice;
}

/* 当前任务的用户号或组号：which 是 ID_UID 等。 */
uint32_t sched_id(int which)
{
    struct task *t = &tasks[current_task];
    return which == ID_UID ? t->uid : which == ID_EUID ? t->euid : which == ID_GID ? t->gid : t->egid;
}

/* 有效用户号是 0：root 做什么都不查权限。Linux 0.11 的 suser() 也是这样。 */
int suser(void)
{
    return tasks[current_task].euid == 0;
}

/*
 * setuid：有效用户号是 root 的，把实际、有效、保存的三个用户号都换成 uid，从此再也变不回 root（su、login 就这样换成别人）；
 * 不是 root 的，只能把有效用户号换成实际的或保存的那一个。按 POSIX 的规定。
 * Linux 0.11 的 sys_setuid() 是 setreuid(uid, uid)，没有保存的用户号；它只在 uid > 0 时才改，setuid(0) 什么也不做。
 */
int sched_setuid(uint32_t uid)
{
    struct task *t = &tasks[current_task];
    if (uid > 0xFFFF)
        return -1;
    if (t->euid == 0)
        t->uid = t->euid = t->suid = (uint16_t)uid;
    else if (uid == t->uid || uid == t->suid)
        t->euid = (uint16_t)uid;
    else
        return -1;
    return 0;
}

/* setgid：同 setuid，看的也是有效用户号是不是 root。 */
int sched_setgid(uint32_t gid)
{
    struct task *t = &tasks[current_task];
    if (gid > 0xFF)                 /* Minix v1 的 i 节点里组号只有一个字节 */
        return -1;
    if (t->euid == 0)
        t->gid = t->egid = t->sgid = (uint16_t)gid;
    else if (gid == t->gid || gid == t->sgid)
        t->egid = (uint16_t)gid;
    else
        return -1;
    return 0;
}

/*
 * exec 换上新程序时：程序文件有 setuid 位，有效用户号换成文件的主人；有 setgid 位，有效组号换成文件的组。
 * 然后把有效的记成保存的：程序可以先换回实际的用户号做事，需要时再换回来。Linux 0.11 的 do_execve() 也这样设 euid、egid。
 */
void sched_exec_ids(int set_uid, uint32_t uid, int set_gid, uint32_t gid)
{
    struct task *t = &tasks[current_task];
    if (set_uid)
        t->euid = (uint16_t)uid;
    if (set_gid)
        t->egid = (uint16_t)gid;
    t->suid = t->euid;
    t->sgid = t->egid;
}

/*
 * exec 换上了新程序：记下程序文件和它的段，返回原来的程序文件，由调用者放手（sched_drop_exec 那样）。
 * 调用者拿着文件系统锁，ip 已经 iget 过、execs 加过一。
 */
struct m_inode *sched_set_exec(struct m_inode *ip, const struct segment *segs, int n)
{
    struct task *t = &tasks[current_task];
    struct m_inode *old = t->exec_ip;
    uint32_t flags = irq_save();            /* 缺页处理会读这几项 */
    t->exec_ip = ip;
    t->nsegs = n;
    for (int i = 0; i < n; i++)
        t->segs[i] = segs[i];
    irq_restore(flags);
    return old;
}

/* 当前任务运行的程序文件和它的段；内核建的任务返回 0。 */
struct m_inode *current_exec(const struct segment **segs, int *n)
{
    struct task *t = &tasks[current_task];
    *segs = t->segs;
    *n = t->nsegs;
    return t->exec_ip;
}

/*
 * 别的任务正在运行同一个程序文件 ip，而且已经装入了 va 这一页：返回那个物理页，引用次数加一；没有返回 0。
 * 调用者关着中断：找到以后、加上引用之前，那个任务不会结束、把页还回去。
 */
uint32_t sched_share_page(struct m_inode *ip, uint32_t va)
{
    for (int n = 1; n < NR_TASKS; n++) {
        if (n == current_task || tasks[n].exec_ip != ip || tasks[n].state == TASK_UNUSED || tasks[n].state == TASK_ZOMBIE)
            continue;
        uint32_t page = user_page_phys(tasks[n].page_dir, va);
        if (page) {
            page_add_ref(page);
            return page;
        }
    }
    return 0;
}

/* 不再运行这个程序了（exec 换了别的、或者要结束了）：放开程序文件。会拿文件系统锁，要在能睡的时候调用。 */
void sched_drop_exec(void)
{
    struct task *t = &tasks[current_task];
    struct m_inode *ip = t->exec_ip;
    if (!ip)
        return;
    fs_lock();
    uint32_t flags = irq_save();
    t->exec_ip = 0;
    t->nsegs = 0;
    irq_restore(flags);
    ip->execs--;
    iput(ip);                       /* 程序文件已经删掉了的话，最后一个运行它的结束时才真正收回 */
    fs_unlock();
}

/* times 系统调用：自己和收走的子进程用了多少 CPU 时间，返回开机以来的 tick 数。Linux 0.11 的 sys_times() 也返回 jiffies。 */
uint32_t sched_times(struct tms *buf)
{
    uint32_t flags = irq_save();        /* 定时器中断随时在加：四个数要是同一时刻的 */
    struct task *t = &tasks[current_task];
    *buf = (struct tms){ t->utime, t->stime, t->cutime, t->cstime };
    uint32_t now = timer_ticks;
    irq_restore(flags);
    return now;
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
    tasks[n] = (struct task){ .state = TASK_RUNNING, .page_dir = cr3, .kstack = kstack, .cwd = "/", .pgrp = n, .sid = n,
                              .counter = DEF_PRIORITY, .priority = DEF_PRIORITY };
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
 * 新的用户任务怎么开始：TSS 里填的是特权级 0 的现场，EIP 是 isr_return，ESP 指着内核栈顶上摆好的 frame。
 * 第一次被调度时，它像刚处理完一次中断那样弹出 frame 里的段寄存器和通用寄存器，iretd 进入用户态。
 * frame 里是用户态的 CS、EIP、EFLAGS、SS、ESP：init 的入口，或者 fork 时父进程进入系统调用的那个现场。
 * 于是每一次硬件任务切换都是从特权级 0 切到特权级 0，用户态只靠 iretd 进入。
 * Linux 0.11 的 copy_process() 把用户态的寄存器直接填进 TSS，任务切换时 CPU 直接进入特权级 3。真 CPU 这样没问题；
 * 可 v86（make web 用的浏览器里的模拟器）在任务切换的中途就把特权级换成 3，再去读 TSS 和 GDT，这两处都只有内核能访问，
 * 读的时候缺页，它处理不了，就停了。Linux 0.11 把前 16 MB 都映射成用户可访问，碰不到这个问题；这里只有 .user 段对用户可见。
 * 后来的 Linux 也是这样起步的新进程：ret_from_fork。
 * ss0:esp0 是它的内核栈：在用户态发生中断或系统调用时，CPU 从这里取出内核栈，再把用户态的 SS、ESP、EFLAGS、CS、EIP 压上去。
 */
static void start_in_kernel(int n, const struct interrupt_frame *frame)
{
    struct interrupt_frame *top = (struct interrupt_frame *)(tasks[n].kstack + PAGE_SIZE) - 1;
    for (uint32_t i = 0; i < sizeof(*top) / 4; i++)     /* 逐字复制：结构体赋值会被编译成调用 memmove，内核里没有它 */
        ((uint32_t *)top)[i] = ((const uint32_t *)frame)[i];
    struct tss *t = &tss[n];
    t->eip = (uint32_t)isr_return;
    t->eflags = 0x002;              /* 弹 frame 时关着中断：iretd 换上 frame 里的 EFLAGS（IF=1） */
    t->esp = (uint32_t)top;
    t->cs = KERNEL_CODE_SEL;
    t->ds = t->es = t->fs = t->gs = t->ss = KERNEL_DATA_SEL;
    t->ss0 = KERNEL_DATA_SEL;
    t->esp0 = tasks[n].kstack + PAGE_SIZE;
}

/*
 * 用户任务：有自己的页目录。frame 里的 CS 等是用户段选择子（RPL 为 3），EFLAGS 的 IOPL 为 0，用户态不能执行 cli、in、out。
 * Linux 0.11 的 move_to_user_mode() 也是在栈上摆一个返回现场，用 iret 落到特权级 3。
 */
static void init_user_task(int n, void (*entry)(void), uint32_t user_esp)
{
    uint32_t dir = new_page_dir();

    if (dir == 0 || !setup_task(n, entry, dir))
        panic("no memory for a user task");
    tasks[n].start_brk = tasks[n].brk = USER_LOW;   /* init 的代码在内核映像里，用户空间里还什么都没有 */
    struct interrupt_frame f = { .gs = USER_DATA_SEL, .fs = USER_DATA_SEL, .es = USER_DATA_SEL, .ds = USER_DATA_SEL,
                                 .eip = (uint32_t)entry, .cs = USER_CODE_SEL, .eflags = 0x202,
                                 .user_esp = user_esp, .user_ss = USER_DATA_SEL };
    start_in_kernel(n, &f);
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
    tasks[0] = (struct task){ .state = TASK_RUNNING, .page_dir = kernel_page_dir(), .cwd = "/", .priority = DEF_PRIORITY };
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
    scheduling = 1;                 /* 此刻运行的是任务 0，下一个 tick 就切走：A、B 的 counter 一样多，先挑任务号大的 B */
}

/*
 * 远跳转到 TSS 选择子：偏移被忽略，CPU 执行任务切换。要切到的任务一定停在特权级 0（见 start_in_kernel()）：
 * 切换只在内核里发生，CPU 存进 TSS 的就是内核态的现场。
 */
static void switch_to(int next)
{
    if (tss[next].cs & 3)
        panic("task switch straight into user mode");
    struct {
        uint32_t offset;
        uint16_t selector;
    } __attribute__((packed)) target = { 0, (uint16_t)TSS_SEL(next) };

    current_task = next;
    __asm__ volatile("ljmp *%0" : : "m"(target) : "memory");
    /* 下次切换回本任务时，从这里继续执行。 */
}

/*
 * 挑下一个任务：能运行的任务里，这一轮剩下的 tick（counter）最多的那个，一样多时挑任务号大的（可能就是当前任务自己）。
 * 能运行的都用完了，就开始新的一轮：每个任务（睡着的也算）的 counter 变成 counter / 2 + priority，再挑一次。
 * 睡着的任务最多攒到两倍 priority 左右。一个能运行的都没有时返回 0：任务 0 是 kmain 的 hlt 循环，正好当空闲任务。
 * Linux 0.11 的 schedule() 一模一样：从任务表的末尾往前找 counter 最大的，c 为 0 时重新算一遍所有任务的 counter。
 */
static int pick_next(void)
{
    for (;;) {
        int next = 0, c = -1;
        for (int n = NR_TASKS - 1; n > 0; n--)
            if (tasks[n].state == TASK_RUNNING && tasks[n].counter > c)
                c = tasks[n].counter, next = n;
        if (c != 0)                 /* 找到了（c > 0），或者一个能运行的都没有（c 为 -1） */
            return next;
        for (int n = 1; n < NR_TASKS; n++)
            if (tasks[n].state != TASK_UNUSED)
                tasks[n].counter = tasks[n].counter / 2 + tasks[n].priority;
    }
}

/* 切到下一个能运行的任务。 */
static void reschedule(void)
{
    int next = pick_next();

    if (next != current_task)       /* 跳到自己的 TSS 会因为它“忙”而触发 #GP */
        switch_to(next);
}

static void send_signal(int n, int sig);

/*
 * 叫醒到时间的任务，给闹钟到了的任务发 SIGALRM（它要是在睡，信号也会把它叫醒）。用差值比较，timer_ticks 回绕到 0 后也对。
 * Linux 0.11 的 schedule() 开头也是这样查一遍每个任务的 alarm。
 */
static void wake_sleepers(void)
{
    for (int n = 1; n < NR_TASKS; n++) {
        if (tasks[n].state == TASK_SLEEPING && (int32_t)(timer_ticks - tasks[n].wake_at) >= 0)
            tasks[n].state = TASK_RUNNING;
        if (tasks[n].alarm_on && (int32_t)(timer_ticks - tasks[n].alarm_at) >= 0) {
            tasks[n].alarm_on = 0;
            if (tasks[n].state != TASK_ZOMBIE && tasks[n].state != TASK_UNUSED)
                send_signal(n, SIGALRM);
        }
    }
}

/*
 * 每个 tick 看一眼此刻谁占着 CPU：是任务 0 就记一次空闲，否则给它记一个 tick 的 utime（打断的是用户态）或 stime。
 * 每秒在第 4 行显示一次空闲比例。抽样难免有误差：一个 tick 里运行过好几个任务，只记给中断时的那一个。
 * Linux 0.11 的 do_timer() 用同样的抽样办法：cpl 不为 0 记 utime，否则记 stime。
 */
static void account(int user)
{
    static uint32_t idle, total;
    char buf[12];

    if (current_task == 0)
        idle++;
    else if (user)
        tasks[current_task].utime++;
    else
        tasks[current_task].stime++;
    if (++total < TIMER_HZ)
        return;
    const char *digits = format_uint(idle * 100 / total, 10, buf);
    int len = 0;
    while (digits[len])
        len++;
    vga_write_at(IDLE_ROW, IDLE_COL, "idle:    %", VGA_GRAY);
    vga_write_at(IDLE_ROW, IDLE_COL + 9 - len, digits, VGA_GRAY);   /* 百分数右对齐在 % 前面 */
    clock_show();                   /* 每秒也刷新一次右边的时间 */
    idle = total = 0;
}

/*
 * 由定时器中断调用：记 CPU 时间、叫醒到时间的任务，再决定要不要切换：空闲的任务 0 在运行就马上看看有没有别人能运行，
 * 否则当前任务的 counter 减一，减到 0 才切换。叫醒的任务 counter 再大也要等当前任务用完，与 Linux 0.11 相同。
 */
void schedule(int user)
{
    if (!scheduling)
        return;
    account(user);
    wake_sleepers();
    if (current_task == 0) {
        reschedule();
    } else if (--tasks[current_task].counter <= 0) {
        tasks[current_task].counter = 0;
        reschedule();
    }
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

    if (sig == SIGCONT || sig == SIGKILL) {
        /*
         * SIGCONT 总是让停着的进程接着运行，不管它怎么处理这个信号；还没处理的停下的信号也作废。
         * SIGKILL 也要先让它接着运行，它才能在回到用户态之前被结束。
         */
        if (sig == SIGCONT)
            t->signal &= ~STOP_SIGNALS;
        if (t->state == TASK_STOPPED) {
            t->state = TASK_RUNNING;
            t->stopsig = 0;
        }
        if (sig == SIGCONT && t->action[sig - 1] <= (uint32_t)SIG_IGN)
            return;                 /* 没设处理函数：接着运行就是全部，不用再挂上 */
    }
    if (bit & STOP_SIGNALS)
        t->signal &= ~(1u << (SIGCONT - 1));    /* 停下的信号让没处理的 SIGCONT 作废 */
    if (t->action[sig - 1] == (uint32_t)SIG_IGN && sig != SIGSTOP)
        return;
    if (sig == SIGCHLD && t->action[sig - 1] == (uint32_t)SIG_DFL)
        return;                     /* SIGCHLD 默认不理 */
    t->signal |= bit;
    if (t->state == TASK_SLEEPING || t->state == TASK_WAITING || (t->state == TASK_BLOCKED && t->interruptible))
        t->state = TASK_RUNNING;
}

/*
 * 当前任务能不能给任务 n 发信号：root 能给谁都发；别人要自己的实际或有效用户号，等于对方的实际或保存的用户号。
 * 所以普通用户能结束自己运行的 su（实际用户号是自己），结束不了 root 的程序。按 POSIX 的规定；
 * Linux 0.11 的 send_sig() 只比两边的有效用户号。终端发的 Ctrl+C 不经过这里，不查。
 */
static int may_signal(int n)
{
    const struct task *me = &tasks[current_task], *t = &tasks[n];
    return me->euid == 0 || me->uid == t->uid || me->uid == t->suid || me->euid == t->uid || me->euid == t->suid;
}

/*
 * kill 系统调用：pid > 0 发给那个任务；pid 为 0 发给自己所在的进程组（自己也收到）；pid < -1 发给 -pid 那一组。
 * 任务 0、A 和 init 收不到：前两个是内核任务，从不回到用户态，信号永远处理不了；init 结束了就没人收养孤儿、拉起 shell。
 * 一个也没找到、找到的都没有权限（见 may_signal()）返回 -1。Linux 0.11 的 sys_kill() 一样按 pid 分几种情况；pid 为 -1 时发给所有进程，这里不支持。
 */
int sched_kill(int pid, int sig)
{
    if (sig < 1 || sig > 32 || pid == -1)
        return -1;
    uint32_t flags = irq_save();
    int group = pid == 0 ? tasks[current_task].pgrp : -pid, sent = 0;

    for (int n = INIT_TASK + 1; n < NR_TASKS; n++) {
        if (tasks[n].state == TASK_UNUSED || (pid > 0 ? n != pid : tasks[n].pgrp != group) || !may_signal(n))
            continue;
        sent = 1;
        if (tasks[n].state != TASK_ZOMBIE)  /* 已经结束了，信号也没用了 */
            send_signal(n, sig);
    }
    irq_restore(flags);
    return sent ? 0 : -1;               /* 一个也没找到，或者找到的都没有权限 */
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
    if (sig < 1 || sig > 32 || sig == SIGKILL || sig == SIGSTOP)   /* 这两个谁也拦不住 */
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
    t->sid = current_task;
    return current_task;
}

/*
 * setpgid：把 pid（0 是自己）放进进程组 pgid（0 就是 pid 自己的号）。只能改自己或者自己的子进程，
 * 会话的头不能改，要进的组得是同一个会话里已经有的（或者新开一个以自己为号的组）。shell 给每个作业一个进程组：
 * 第一个子进程的号就是组号，后面的子进程都放进去。Linux 0.11 的 sys_setpgid() 也是这样。
 */
int sched_setpgid(int pid, int pgid)
{
    pid = pid ? pid : current_task;
    pgid = pgid ? pgid : pid;
    if (pid < 1 || pid >= NR_TASKS || pgid < 1 || pgid >= NR_TASKS)
        return -1;
    uint32_t flags = irq_save();
    struct task *t = &tasks[pid];
    int ok = t->state != TASK_UNUSED && t->state != TASK_ZOMBIE && (pid == current_task || t->parent == current_task) &&
             !t->leader && t->sid == tasks[current_task].sid;
    if (ok && pgid != pid) {
        ok = 0;
        for (int n = 1; n < NR_TASKS; n++)
            if (tasks[n].state != TASK_UNUSED && tasks[n].pgrp == pgid && tasks[n].sid == t->sid)
                ok = 1;
    }
    if (ok)
        t->pgrp = pgid;
    irq_restore(flags);
    return ok ? 0 : -1;
}

int sched_sid(void)
{
    return tasks[current_task].sid;
}

/* 会话 sid 里有没有进程组 pgrp：设终端的前台进程组时检查 */
int sched_pgrp_in_session(int pgrp, int sid)
{
    for (int n = 1; n < NR_TASKS; n++)
        if (tasks[n].state != TASK_UNUSED && tasks[n].state != TASK_ZOMBIE && tasks[n].pgrp == pgrp && tasks[n].sid == sid)
            return 1;
    return 0;
}

/*
 * 当前任务停下（SIGSTOP、SIGTSTP 这些的默认动作，或者后台读终端）：状态改成停着，在 waitpid 里等着的父进程叫醒，
 * 让它看到；切走，等 SIGCONT 把它改回能运行，才从这里接着往下走。Linux 0.11 的 do_signal() 不认停下的信号。
 */
void sched_stop(int sig)
{
    uint32_t flags = irq_save();
    struct task *t = &tasks[current_task];
    t->state = TASK_STOPPED;
    t->stopsig = sig;
    t->stop_reported = 0;
    if (t->parent && tasks[t->parent].state == TASK_WAITING)
        tasks[t->parent].state = TASK_RUNNING;
    while (t->state == TASK_STOPPED)
        reschedule();
    irq_restore(flags);
}

/* 第 n 个任务的用户页表（给交换的时钟用）：没有用户空间返回 0；*is_current 说明它是不是当前任务（改了要刷新 TLB） */
uint32_t *sched_user_table(int n, int *is_current)
{
    struct task *t = &tasks[n];
    if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE || !t->page_dir || t->page_dir == kernel_page_dir())
        return 0;
    uint32_t pde = ((uint32_t *)t->page_dir)[USER_LOW >> 22];
    if (!(pde & 1))
        return 0;
    *is_current = n == current_task;
    return (uint32_t *)(pde & ~0xFFFu);
}

/* 当前任务有没有挂着、还没处理的信号 */
int sched_signal_pending(void)
{
    return tasks[current_task].signal != 0;
}

/* 当前任务对信号 sig 是怎么处理的：SIG_DFL、SIG_IGN 或者处理函数的地址 */
uint32_t sched_action(int sig)
{
    return tasks[current_task].action[sig - 1];
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
 * 打开终端时调用：进程组的头第一次打开一个终端，那个终端就成了它的控制终端，返回 1，调用者把终端的前台进程组设成这一组。
 * 已经有了控制终端的再打开不算，不会把前台进程组抢过来。Linux 0.11 的 sys_open() 也要求 current->tty < 0。
 */
int sched_claim_tty(int tty)
{
    struct task *t = &tasks[current_task];

    if (!t->leader || t->ctty)
        return 0;
    t->ctty = tty + 1;
    return 1;
}

/*
 * alarm 系统调用：seconds 秒之后给自己发 SIGALRM；0 是取消。返回上一个闹钟还剩几秒（向上取整，没有就是 0）。
 * 一个任务只有一个闹钟，新定的替掉旧的。Linux 0.11 的 sys_alarm() 一样，只是剩下的秒数向下取整。
 */
uint32_t sched_alarm(uint32_t seconds)
{
    struct task *t = &tasks[current_task];
    uint32_t flags = irq_save();
    uint32_t old = 0;

    if (t->alarm_on) {
        int32_t left = (int32_t)(t->alarm_at - timer_ticks);
        old = left > 0 ? ((uint32_t)left + TIMER_HZ - 1) / TIMER_HZ : 0;
    }
    if (seconds > 0x7FFFFFFF / TIMER_HZ)    /* wake_sleepers() 用有符号的差值比较 */
        seconds = 0x7FFFFFFF / TIMER_HZ;
    t->alarm_on = seconds != 0;
    t->alarm_at = timer_ticks + seconds * TIMER_HZ;
    irq_restore(flags);
    return old;
}

/*
 * pause 系统调用：睡下，直到来了一个信号，返回 -EINTR。信号设了处理函数的，处理完 pause 才返回；
 * 没设的，进程就此结束。和 sleep 一样是可以被信号打断的睡眠，ps 里是 S。
 * Linux 0.11 的 sys_pause() 只把状态设成 TASK_INTERRUPTIBLE、调度一次，醒来就返回 0：能叫醒这种睡眠的
 * 只有信号，效果差不多。这里醒来再查一遍有没有信号，没有就接着睡；返回 -EINTR 是 POSIX 的规定。
 */
int sched_pause(void)
{
    uint32_t flags = irq_save();
    while (!tasks[current_task].signal) {
        tasks[current_task].state = TASK_SLEEPING;
        tasks[current_task].wake_at = timer_ticks + 0x7FFFFFFF;   /* 等于永远：只有信号能叫醒它 */
        reschedule();
    }
    irq_restore(flags);
    return -EINTR;
}

/* exec 装好新程序：堆从它的末尾开始，是空的。 */
void sched_set_brk(uint32_t end)
{
    tasks[current_task].start_brk = tasks[current_task].brk = end;
}

/*
 * brk 系统调用：把堆的末尾挪到 addr。不能低于程序的末尾，最高到栈区往下 16 KB（BRK_LIMIT）；做不到就不动。返回现在的堆末尾，
 * 所以 brk(0) 就是问“堆现在到哪儿”。挪高了并不马上分内存：用到哪一页，缺页时才补哪一页。
 * 挪低了也不收回已经分了的页，进程结束时一起收回。Linux 0.11 的 sys_brk() 也只是记下这个数，
 * 它允许的范围是代码的末尾到栈底往下 16 KB；这里的栈区固定是顶端的 64 KB，下面同样留 16 KB 的空地。
 */
uint32_t sched_brk(uint32_t addr)
{
    struct task *t = &tasks[current_task];

    if (addr >= t->start_brk && addr <= BRK_LIMIT)
        t->brk = addr;
    return t->brk;
}

/* 缺页时用：当前任务的堆到哪儿。 */
uint32_t current_brk(void)
{
    return tasks[current_task].brk;
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
 * 父进程进入系统调用时 CPU 和 isr_common 压在栈上的寄存器（frame）原样复制到新任务的内核栈顶，只把 EAX 改成 0：
 * EIP 是 int 0x80 的下一条指令，CS、SS、ESP 是用户态的。新任务第一次被调度时从 isr_return 弹出它、iretd 回到用户态，
 * 好像 fork 刚返回 0（见 start_in_kernel()）。
 * Linux 0.11 的 copy_process() 也是用栈上的寄存器、把 eax 设为 0，只是直接填进新进程的 TSS。
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
    tss[n] = (struct tss){ .cr3 = dir, .iomap_base = sizeof(struct tss) };
    tasks[n] = (struct task){ .state = TASK_RUNNING, .page_dir = dir, .kstack = kstack, .parent = current_task,
                              .restorer = tasks[current_task].restorer, .pgrp = tasks[current_task].pgrp,   /* 收到的信号不继承 */
                              .sid = tasks[current_task].sid,
                              .start_brk = tasks[current_task].start_brk, .brk = tasks[current_task].brk,
                              .cloexec = tasks[current_task].cloexec,
                              .priority = tasks[current_task].priority, .counter = tasks[current_task].priority,
                              .uid = tasks[current_task].uid, .euid = tasks[current_task].euid, .suid = tasks[current_task].suid,
                              .gid = tasks[current_task].gid, .egid = tasks[current_task].egid, .sgid = tasks[current_task].sgid,
                              .exec_ip = tasks[current_task].exec_ip, .nsegs = tasks[current_task].nsegs };
    for (int i = 0; i < MAX_SEGS; i++)
        tasks[n].segs[i] = tasks[current_task].segs[i];
    struct interrupt_frame child = *frame;
    child.eax = 0;                  /* 子进程里 fork 返回 0 */
    start_in_kernel(n, &child);
    if (tasks[n].exec_ip) {         /* 子进程运行的是同一个程序：多一个引用。调用者拿着文件系统锁（见 sys_fork） */
        tasks[n].exec_ip->count++;
        tasks[n].exec_ip->execs++;
    }
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
            if (tasks[n].state == TASK_STOPPED && (options & WUNTRACED) && !tasks[n].stop_reported) {
                tasks[n].stop_reported = 1;     /* 一次停下只报告一次 */
                uint32_t st = EXIT_STOPPED | (uint32_t)tasks[n].stopsig;
                irq_restore(flags);
                if (status)
                    *status = st;
                return n;
            }
            if (tasks[n].state == TASK_ZOMBIE) {
                uint32_t st = tasks[n].status;
                /* 子进程用的 CPU 时间算到父进程的 cutime、cstime 里。Linux 0.11 只加子进程自己的，不加孙子进程的 */
                tasks[current_task].cutime += tasks[n].utime + tasks[n].cutime;
                tasks[current_task].cstime += tasks[n].stime + tasks[n].cstime;
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
 *   - 用户内存当场收回：用户空间里的页（程序、堆、栈）、页表和页目录。
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
    sched_drop_exec();              /* 和关文件一样要拿文件系统锁，趁还能睡的时候做 */
    if (t->leader && t->ctty)
        tty_release_pgrp(t->ctty - 1, t->sid);   /* 终端的会话没了头：Ctrl+C 不再发给谁。Linux 0.11 的 do_exit() 也这样 */
    __asm__ volatile("cli");        /* 不再返回，切到的任务从自己的 TSS 装入 EFLAGS，中断状态随之恢复 */
    if (t->page_dir != kernel_page_dir())
        free_page_dir(t->page_dir);
    t->page_dir = 0;
    for (int n = 1; n < NR_TASKS; n++) {
        if (tasks[n].state != TASK_UNUSED && tasks[n].parent == current_task) {
            tasks[n].parent = 0;
            if (tasks[n].state == TASK_ZOMBIE)
                release(n);
            else if (tasks[n].state == TASK_STOPPED) {
                /*
                 * 停着的孩子没人管了（比如 shell 退出了，留下 Ctrl+Z 停下的作业）：再也没人会让它接着运行。
                 * POSIX 的规定是给它 SIGHUP 再 SIGCONT：接着运行，马上被 SIGHUP 结束。
                 */
                send_signal(n, SIGHUP);
                send_signal(n, SIGCONT);
            }
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
