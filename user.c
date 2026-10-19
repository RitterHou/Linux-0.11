/*
 * 用户程序：任务 B 在特权级 3（用户态）运行这里的代码。
 *
 * 链接脚本把本文件的代码、只读数据和数据单独放进按 4 KB 对齐的 .user 段，
 * paging.c 只给这几页打开“用户”位，其中代码所在的页只读。用户栈在 0x40000000 起的 4 MB 的顶端 64 KB 里，
 * 用到哪一页，内核才在缺页异常里分配哪一页（见 paging.c 的 handle_page_fault()）。
 * 这几页是内核映像的一部分，所有任务共用：这里的全局变量 fork 时不复制。内核的代码、数据和显存对这里的代码都不可访问，
 * cli、hlt、in、out 等特权指令也不能执行，要做任何事都只能用 int 0x80 请内核帮忙。
 * 所以这里只包含 syscall.h，不包含内核的 kernel.h。
 */
#include "selftest.h"
#include "syscall.h"

/*
 * 信号处理函数返回到这里：内核把它的地址放在处理函数的返回地址上（见 interrupt.c 的 handle_signals()）。
 * 此时栈上依次是信号编号、eax、ecx、edx、EFLAGS、被打断的地方：跳过信号编号，弹回这几个寄存器，
 * 再 ret 回去。Linux 0.11 的 libc 里的 __sig_restore 一模一样。和 user/ulib.c 里的相同：
 * 这个文件链接进内核映像，用不到硬盘上程序的函数库。
 */
__asm__(".globl sig_restore\n"
        "sig_restore:\n"
        "    addl $4, %esp\n"
        "    popl %eax\n"
        "    popl %ecx\n"
        "    popl %edx\n"
        "    popfl\n"
        "    ret\n");

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_SIGNAL
/*
 * 注意：这个文件链接进内核映像，这些全局变量在内核映像的 .user 数据区里，一一对应映射，所有任务共用，
 * fork 时不复制（只有 0x40000000 起的用户空间是各有一份的）。所以每一项开始前，init 先把它们清零。
 */
static volatile int32_t caught, caught_count;
static volatile uint32_t handler_flags;

static void on_signal(int sig)
{
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    handler_flags = flags;
    caught = sig;
    caught_count++;
}

/* exec 成功之后，任务的名字就是新程序的文件名。 */
static int is_cat(int32_t pid)
{
    struct taskinfo info;
    return taskinfo(pid, &info) == 0 && info.name[0] == 'c' && info.name[1] == 'a' && info.name[2] == 't' && info.name[3] == 0;
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_BRK
/* 在子进程里运行 /bin/sort：in 从一个管道喂给它的标准输入，它的标准输出从另一个管道读回 out。返回读到的字节数，sort 失败返回 -1。 */
static int32_t run_sort(char **argv, const char *in, uint32_t in_len, char *out, uint32_t cap)
{
    int32_t a[2], b[2], st = -1;
    pipe(a);
    pipe(b);
    int32_t c = fork();
    if (c == 0) {
        if (a[0] != 0) {
            close(0);
            dup(a[0]);
            close(a[0]);
        }
        close(1);
        dup(b[1]);                      /* 0 号已经占着：dup 返回的正是刚关掉的 1 号 */
        close(b[1]);
        close(b[0]);
        close(a[1]);
        execv("/bin/sort", argv);
        exit(9);
    }
    close(a[0]);
    close(b[1]);
    write(a[1], in, in_len);            /* 不到一页，管道放得下 */
    close(a[1]);
    uint32_t got = 0;
    int32_t n;
    while (got < cap && (n = read(b[0], out + got, cap - got)) > 0)
        got += (uint32_t)n;
    close(b[0]);
    waitpid(c, &st, 0);
    return st == 0 ? (int32_t)got : -1;
}

static int same(const char *a, const char *b, int32_t n)
{
    for (int32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void put_file(const char *path, const char *text, uint32_t n)
{
    int32_t fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    write(fd, text, n);
    close(fd);
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_ALARM
static volatile int32_t rang;       /* 内核映像里的全局变量，init 和子进程共用：只在 init 里用 */

static void on_alarm(int sig)
{
    rang = sig;
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_NICE
/* 子进程把自己用的 CPU 时间写在这里：内核映像里的全局变量，所有任务共用，fork 时不复制 */
static volatile uint32_t spent_user[2], spent_sys[2], whole[2];

/*
 * fork 一个子进程：它先 nice(inc)，然后一直空转，把 tick 数从 from 到 until 这一段用的 CPU 时间记进 spent_*[k]，
 * 一共用的记进 whole[k]，再退出。from 之前的那一段不算：子进程刚 fork 出来时 counter 是父进程的 priority，
 * nice 要到下一轮才起作用。in_kernel 为 1 时空转的是 stat 系统调用（时间多花在内核里），否则是用户态的循环。
 */
static int32_t spinner(int k, int32_t inc, uint32_t from, uint32_t until, int in_kernel)
{
    int32_t pid = fork();
    if (pid != 0)
        return pid;
    nice(inc);
    struct tms t0, t;
    struct stat st;
    for (int measuring = 0; measuring < 2; measuring++) {
        while (times(measuring ? &t : &t0) < (measuring ? until : from)) {
            if (in_kernel)
                stat("/bin/sh", &st);
            else                    /* 每转 10 万圈才问一次 times：在 QEMU 里，一次系统调用比上千圈空循环还慢 */
                for (volatile int i = 0; i < 100000; i++)
                    ;
        }
    }
    spent_user[k] = t.tms_utime - t0.tms_utime;
    spent_sys[k] = t.tms_stime - t0.tms_stime;
    whole[k] = t.tms_utime + t.tms_stime;
    exit(0);
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_PERM
/* 子进程（换成了用户 100）把每一项的结果记在这里：内核映像里的全局变量，所有任务共用，fork 时不复制 */
static volatile char perm_res[16];
static volatile int perm_n;

static void perm_put(int ok, char letter)
{
    perm_res[perm_n++] = ok ? letter : (char)(letter - 'a' + 'A');
}

/* 把 from 整个复制成 to：没有 cp 命令，也不能 link（chmod 会连原来的一起改） */
static int copy_file(const char *from, const char *to)
{
    char buf[512];
    int32_t in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC), n, ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof(buf))) > 0)
        ok = write(out, buf, (uint32_t)n) == n;
    close(in);
    close(out);
    return ok;
}

/* fork 一个子进程 exec path（标准输出接到管道上，读回 out），返回它的退出状态；exec 失败的是 42 */
static int32_t run_capture(const char *path, char *out, uint32_t cap, uint32_t *got)
{
    int32_t p[2], st = -1;
    pipe(p);
    int32_t c = fork();
    if (c == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *argv[] = { (char *)path, 0 };
        execv(path, argv);
        exit(42);
    }
    close(p[1]);
    int32_t n;
    *got = 0;
    while (*got < cap && (n = read(p[0], out + *got, cap - *got)) > 0)
        *got += (uint32_t)n;
    close(p[0]);
    waitpid(c, &st, 0);
    return st;
}

static int same_text(const char *a, uint32_t n, const char *b)
{
    uint32_t i = 0;
    for (; i < n && b[i]; i++)
        if (a[i] != b[i])
            return 0;
    return i == n && b[i] == '\0';
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_STOP
/* 内核映像里的全局变量，所有任务共用：子进程把结果、孙子进程的号写在这里 */
static volatile int32_t caught_tstp, grandchild;

static void on_tstp(int sig)
{
    caught_tstp = sig;
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_SWAP
/* 从 base 起的 pages 页，每页的头上写 seed 加页号，尾上写它取反 */
static void swap_fill(uint32_t base, uint32_t pages, uint32_t seed)
{
    for (uint32_t i = 0; i < pages; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(base + i * 4096);
        p[0] = seed + i;
        p[1023] = ~(seed + i);
    }
}

/* 读回来核对。volatile：不让 clang 看出刚写过、直接把结果算出来 */
static int swap_check(uint32_t base, uint32_t pages, uint32_t seed)
{
    int ok = 1;
    for (uint32_t i = 0; i < pages; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(base + i * 4096);
        ok &= p[0] == seed + i && p[1023] == ~(seed + i);
    }
    return ok;
}

/* 把堆挪大 pages 页，返回原来的末尾；挪不了返回 0 */
static uint32_t swap_grow(uint32_t pages)
{
    uint32_t base = brk(0);
    return brk(base + pages * 4096) == base + pages * 4096 ? base : 0;
}

/* 两个子进程一起用：各自写三遍、每遍睡一下让对方接着挤，每遍都核对 */
static int swap_churn(uint32_t pages, uint32_t seed)
{
    uint32_t base = swap_grow(pages);
    int ok = base != 0;
    for (uint32_t round = 0; ok && round < 3; round++) {
        swap_fill(base, pages, seed + round * 10000);
        sleep(2);
        ok &= swap_check(base, pages, seed + round * 10000);
    }
    return ok;
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_WRITE_KERNEL
extern volatile uint32_t timer_ticks;   /* 内核的变量，所在的页没有用户位 */
#endif

#if !defined(SELFTEST)
static int32_t start_shell(void)
{
    int32_t pid = fork();
    if (pid == 0) {
        /*
         * 先自己当一个进程组的头，再打开终端：终端的前台进程组就是 shell 这一组，按 Ctrl+C 只发给它们，
         * init 和写 C 的 letter 不在这一组。Linux 0.11 的 init() 也是先 setsid() 再打开 /dev/tty0。
         */
        setsid();
        close(1);
        open("/dev/tty0", 0);
        open("/dev/tty0", 0);
        open("/dev/tty0", 0);
        char *argv[] = { "sh", 0 };
        execv("/bin/sh", argv);
        exit(1);
    }
    return pid;
}

/*
 * 串口上运行 login：和 shell 一样先 setsid() 当进程组的头，再把 /dev/ttyS0 打开三次，它就是这一组的控制终端。
 * login 以 root 运行，问用户名和密码，对了就换成那个用户 exec 他的 shell；shell 退出（或者 login 三次都不对），
 * init 再拉起一个 login。后来的 Unix 是 init 在每个终端上运行 getty，getty 再 exec login，道理相同。
 */
static int32_t start_login(void)
{
    int32_t pid = fork();
    if (pid == 0) {
        setsid();
        close(1);
        open("/dev/ttyS0", O_RDWR);
        open("/dev/ttyS0", O_RDWR);
        open("/dev/ttyS0", O_RDWR);
        char *argv[] = { "login", 0 };
        execv("/bin/login", argv);
        exit(1);
    }
    return pid;
}
#endif

/* 与内核任务 A 的 task_main() 做同样的事：任务 2 写 B；它 fork 出的任务 3 换成 /bin/letter，写 C。 */
void user_main(void)
{
#if !defined(SELFTEST) || !SELFTEST_IN_USER(SELFTEST)
    /* 正常构建：没有用户态自检 */
#elif SELFTEST == SELFTEST_USER_WRITE_KERNEL
    timer_ticks = 0;
#elif SELFTEST == SELFTEST_USER_PRIVILEGED
    __asm__ volatile("cli");
#elif SELFTEST == SELFTEST_USER_BAD_POINTER
    /* 把内核代码的地址交给 write：内核必须拒绝，否则用户程序就能借 write 读出内核内存 */
    int32_t ret = write(1, (const void *)0x10000, 1);
    write(1, ret == -1 ? "K" : "X", 1);
#elif SELFTEST == SELFTEST_USER_EXIT
    write(1, "bye", 3);
    exit(7);
#elif SELFTEST == SELFTEST_USER_WRITE_CODE
    *(volatile uint8_t *)user_main = 0xC3;  /* 改写自己的代码：代码页只读，期望因 #PF 被结束 */
#elif SELFTEST == SELFTEST_USER_SPIN
    /* 做一次系统调用（让内核写出 U），然后一直在用户态空转，便于测试在用户态注入 NMI */
    write(1, "s", 1);
    for (;;)
        __asm__ volatile("");
#elif SELFTEST == SELFTEST_USER_DEEP_STACK
    /*
     * 在栈上用掉 5 页：碰到的每一页第一次都缺页，内核补上一页。最低的那一页自己不碰，
     * 直接交给 write，由内核去读：内核态碰到栈区里没映射的页也要补上，write 返回 1，再写一个 k。
     * 最后 exit(3)，内核收回所有栈页和页表。
     */
    volatile char deep[5 * 4096];
    for (int i = 1; i < 5; i++)
        deep[i * 4096] = 'd';
    int32_t ret = write(1, (const void *)deep, 1);
    write(1, ret == 1 ? "k" : "x", 1);
    exit(3);
#elif SELFTEST == SELFTEST_USER_WILD_READ
    (void)*(volatile uint32_t *)0x50000000;     /* 不在栈区：内核不能补页，只能结束任务 B */
#elif SELFTEST == SELFTEST_USER_FORK
    /*
     * fork 之后父子各改各的：子进程把栈上的 shared 改成 99，父进程看到的仍是 7。
     * 子进程写 c 后 exit(5)；父进程等它写完，看自己的 shared 没变就写 p，再写出 fork 返回的任务号。
     */
    volatile int shared = 7;
    int32_t pid = fork();
    if (pid == 0) {
        shared = 99;
        write(1, shared == 99 ? "c" : "x", 1);
        exit(5);
    }
    sleep(10);
    write(1, shared == 7 ? "p" : "X", 1);
    char digit = (char)('0' + pid);
    write(1, &digit, 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_WAIT
    /*
     * 每一项检查通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 winrhk：
     *   w：waitpid 返回子进程号，退出码 42 写进父进程的 area[0]。这一页 fork 之前就有，fork 之后
     *      父进程、keeper、quick 三方共用，是只读的：内核写它也要先复制（CR0.WP），
     *   i：所以 keeper 醒来后看到的 area[0] 仍是 1。
     *   n：一直 fork 到失败：16 个任务槽去掉任务 0、A、B，正好能 fork 13 次，第 14 次返回 -1。
     *   r：waitpid(-1) 把这 13 个子进程一个个收走，退出码都是 7，之后再 waitpid 返回 -1。
     *   h：WNOHANG：子进程还在睡，立刻返回 0；不是自己的子进程（任务 1）返回 -1；再正常等它。
     *   k：被异常结束的子进程，状态是 0x100 加异常号（缺页是 14）。
     */
    volatile int32_t area[3 * 1024];            /* 12 KB：area[0] 和栈顶的局部变量不在同一页 */
    int32_t st = 0;
    area[0] = 1;
    int32_t keeper = fork();
    if (keeper == 0) {
        sleep(30);
        write(1, area[0] == 1 ? "i" : "x", 1);
        exit(0);
    }
    int32_t quick = fork();
    if (quick == 0)
        exit(42);
    int32_t got = waitpid(quick, (int32_t *)&area[0], 0);
    write(1, got == quick && area[0] == 42 ? "w" : "X", 1);
    waitpid(keeper, 0, 0);

    int forked = 0;
    for (;;) {
        int32_t pid = fork();
        if (pid == 0)
            exit(7);
        if (pid < 0)
            break;
        forked++;
    }
    write(1, forked == 13 ? "n" : "N", 1);
    int reaped = 0, all_seven = 1;
    while (waitpid(-1, &st, 0) > 0) {
        reaped++;
        all_seven &= st == 7;
    }
    write(1, reaped == 13 && all_seven ? "r" : "R", 1);

    int32_t slow = fork();
    if (slow == 0) {
        sleep(20);
        exit(9);
    }
    int32_t early = waitpid(slow, &st, WNOHANG);
    int32_t stranger = waitpid(1, &st, 0);
    got = waitpid(slow, &st, 0);
    write(1, early == 0 && stranger == -1 && got == slow && st == 9 ? "h" : "H", 1);

    int32_t victim = fork();
    if (victim == 0)
        (void)*(volatile uint32_t *)0x50000000;
    got = waitpid(victim, &st, 0);
    write(1, got == victim && st == (0x100 | 14) ? "k" : "K", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_FS
    /*
     * 先把 /hello.txt 的内容（去掉换行）写到第 6 行，然后每项检查通过就写一个字母，期望是 oubdscf：
     *   o：不存在的文件、分量超过 14 个字节的路径，open 都返回 -1
     *   u：read 的缓冲区是内核地址，返回 -1
     *   b：读完 600 KB 的 /big（要用到二级间接块），内容逐字节对，读到末尾返回 0
     *   d：读根目录得到一条条目录项，有 hello.txt、big、etc
     *   s：读子目录里的 /etc/issue
     *   c：close 之后 read 返回 -1；8 个描述符用光后 open 返回 -1，close 一个又能 open
     *   f：fork 之后子进程用继承来的描述符接着读，父进程的读位置跟着往后走
     */
    char buf[1024];
    int32_t fd = open("/hello.txt", 0);
    int32_t n = read(fd, buf, sizeof(buf));
    write(1, buf, n > 0 ? (uint32_t)n - 1 : 0);
    close(fd);

    write(1, open("/nothing", 0) < 0 && open("/etc/abcdefghijklmnopq", 0) < 0 ? "o" : "O", 1);
    fd = open("/hello.txt", 0);
    write(1, read(fd, (void *)0x10000, 4) == -1 ? "u" : "U", 1);
    close(fd);

    fd = open("/big", 0);
    uint32_t total = 0, good = 1;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int32_t i = 0; i < n; i++)
            good &= (uint8_t)buf[i] == (uint8_t)((total + (uint32_t)i) * 7 + 3);
        total += (uint32_t)n;
    }
    write(1, good && n == 0 && total == 600 * 1024 ? "b" : "B", 1);
    close(fd);

    fd = open("/", 0);
    int found = 0;
    while (read(fd, buf, 16) == 16) {
        const char *name = buf + 2;
        if (buf[0] == 0 && buf[1] == 0)
            continue;
        if (name[0] == 'h' && name[5] == '.')
            found |= 1;
        else if (name[0] == 'b' && name[1] == 'i' && name[2] == 'g' && name[3] == 0)
            found |= 2;
        else if (name[0] == 'e' && name[1] == 't' && name[2] == 'c' && name[3] == 0)
            found |= 4;
    }
    write(1, found == 7 ? "d" : "D", 1);
    close(fd);

    fd = open("/etc/issue", 0);
    n = read(fd, buf, sizeof(buf));
    write(1, n == 27 && buf[0] == 'L' && buf[26] == '\n' ? "s" : "S", 1);
    close(fd);

    fd = open("/hello.txt", 0);
    close(fd);
    int ok = read(fd, buf, 1) == -1;
    int32_t fds[8];
    int opened = 0;
    while (opened < 8 && (fds[opened] = open("/hello.txt", 0)) >= 0)
        opened++;
    ok &= opened == 7 && open("/hello.txt", 0) == -1;     /* 1 号描述符本来就占着 */
    close(fds[3]);
    ok &= open("/hello.txt", 0) == fds[3];
    for (int i = 0; i < opened; i++)
        close(fds[i]);
    write(1, ok ? "c" : "C", 1);

    fd = open("/hello.txt", 0);
    read(fd, buf, 6);                               /* "Hello " */
    int32_t child = fork();
    if (child == 0) {
        read(fd, buf, 5);                           /* "from " */
        exit(0);
    }
    waitpid(child, 0, 0);
    n = read(fd, buf, 5);                           /* 位置共用：应读到 "the M" */
    write(1, n == 5 && buf[0] == 't' && buf[4] == 'M' ? "f" : "F", 1);
    close(fd);
    exit(0);
#elif SELFTEST == SELFTEST_USER_EXEC
    /*
     * 每项检查通过就写一个字母，期望第 6 行是 o、hello from exec、h、check ok、k、c 连在一起：
     *   o：不存在的文件、不能执行的文件（/hello.txt 没有执行权限）、目录，exec 都返回 -1，程序接着运行
     *   hello from exec：fork 出的子进程 exec /bin/hello，它把参数打印出来；h：父进程等到它以 0 退出
     *   check ok：/bin/check 检查自己的 .data、.bss、argc 和 argv，都对就打印它；k：父进程等到它以 7 退出
     *   c：/bin/check crash 往自己的代码里写，代码页只读，被缺页异常结束（状态 0x10E）
     */
    int32_t st;
    char *none[] = { "x", 0 };
    write(1, execv("/nothing", none) < 0 && execv("/hello.txt", none) < 0 && execv("/etc", none) < 0 ? "o" : "O", 1);

    int32_t pid = fork();
    if (pid == 0) {
        char *argv[] = { "hello", "from", "exec", 0 };
        execv("/bin/hello", argv);
        exit(99);
    }
    write(1, waitpid(pid, &st, 0) == pid && st == 0 ? "h" : "H", 1);

    pid = fork();
    if (pid == 0) {
        char *argv[] = { "check", 0 };
        execv("/bin/check", argv);
        exit(99);
    }
    write(1, waitpid(pid, &st, 0) == pid && st == 7 ? "k" : "K", 1);

    pid = fork();
    if (pid == 0) {
        char *argv[] = { "check", "crash", 0 };
        execv("/bin/check", argv);
        exit(99);
    }
    write(1, waitpid(pid, &st, 0) == pid && st == (0x100 | 14) ? "c" : "C", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_PIPE
    /*
     * 每项检查通过就写一个字母，期望是 pbwed：
     *   p：pipe 返回最小的两个空闲描述符 0 和 2（1 号是第 6 行）；写进 5 个字节，读出同样的 5 个
     *   b：子进程往管道里写 10000 个字节，比一页的缓冲区大，写满了就睡下等；父进程边读边核对，
     *      子进程退出、写的一头全关了之后，read 返回 0
     *   w：子进程以 0 退出
     *   e：读的一头关了，再写返回 -1
     *   d：dup 返回最小的空闲描述符（0 号是刚打开的文件，1 号是第 6 行，所以是 2），和原来的描述符共用读写位置
     */
    int32_t p[2];
    char buf[512];
    int ok = pipe(p) == 0 && p[0] == 0 && p[1] == 2;
    ok &= write(p[1], "hello", 5) == 5 && read(p[0], buf, sizeof(buf)) == 5 && buf[0] == 'h' && buf[4] == 'o';
    write(1, ok ? "p" : "P", 1);

    int32_t child = fork();
    if (child == 0) {
        close(p[0]);
        for (int k = 0; k < 100; k++) {
            for (int i = 0; i < 100; i++)
                buf[i] = (char)((k * 100 + i) * 13);
            if (write(p[1], buf, 100) != 100)
                exit(1);
        }
        exit(0);
    }
    close(p[1]);
    uint32_t total = 0;
    int good = 1;
    int32_t n;
    while ((n = read(p[0], buf, sizeof(buf))) > 0) {
        for (int32_t i = 0; i < n; i++)
            good &= (uint8_t)buf[i] == (uint8_t)((total + (uint32_t)i) * 13);
        total += (uint32_t)n;
    }
    write(1, good && n == 0 && total == 10000 ? "b" : "B", 1);
    int32_t st = -1;
    write(1, waitpid(child, &st, 0) == child && st == 0 ? "w" : "W", 1);
    close(p[0]);

    pipe(p);
    close(p[0]);
    write(1, write(p[1], "x", 1) == -1 ? "e" : "E", 1);
    close(p[1]);

    int32_t fd = open("/hello.txt", 0);
    int32_t copy = dup(fd);
    ok = fd == 0 && copy == 2 && read(fd, buf, 6) == 6 && read(copy, buf, 4) == 4 && buf[0] == 'f' && buf[3] == 'm';
    close(fd);
    close(copy);
    write(1, ok ? "d" : "D", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_FSWRITE
    /*
     * 每项检查通过就写一个字母，期望是 emtuds：
     *   e：O_CREAT | O_EXCL 遇到已经存在的文件失败，新文件成功
     *   m：只读打开的不能写，只写打开的不能读，目录不能打开来写
     *   t：从头写会覆盖原来的内容；O_TRUNC 清空；O_APPEND 接在末尾
     *   u：删掉一个还开着的文件：目录里看不见了，开着的描述符照样能读；关掉以后块和 i 节点才收回
     *   d：已经存在的目录不能再建；非空目录、根目录、. 删不掉；父目录不存在、名字超过 14 个字节，建不了
     *   s：同一个文件打开两次，共用内存里的 i 节点：从一个描述符写进去，另一个马上读得到
     */
    char buf[64];
    struct fsinfo before, after;
    fsinfo(&before);

    int ok = open("/hello.txt", O_RDWR | O_CREAT | O_EXCL) < 0;
    int32_t fd = open("/tmp/new", O_RDWR | O_CREAT | O_EXCL);
    ok &= fd >= 0 && write(fd, "hello", 5) == 5;
    close(fd);
    write(1, ok ? "e" : "E", 1);

    fd = open("/tmp/new", O_RDONLY);
    ok = write(fd, "x", 1) == -1;
    close(fd);
    fd = open("/tmp/new", O_WRONLY);
    ok &= read(fd, buf, 1) == -1;
    close(fd);
    ok &= open("/tmp", O_WRONLY) < 0 && open("/tmp", O_RDONLY) >= 0;
    write(1, ok ? "m" : "M", 1);

    fd = open("/tmp/new", O_WRONLY);
    write(fd, "HE", 2);
    close(fd);
    fd = open("/tmp/new", O_RDONLY);
    ok = read(fd, buf, sizeof(buf)) == 5 && buf[0] == 'H' && buf[1] == 'E' && buf[2] == 'l';
    close(fd);
    fd = open("/tmp/new", O_WRONLY | O_TRUNC);
    write(fd, "ab", 2);
    close(fd);
    fd = open("/tmp/new", O_WRONLY | O_APPEND);
    write(fd, "cd", 2);
    close(fd);
    fd = open("/tmp/new", O_RDONLY);
    ok &= read(fd, buf, sizeof(buf)) == 4 && buf[0] == 'a' && buf[3] == 'd';
    close(fd);
    write(1, ok ? "t" : "T", 1);

    fd = open("/tmp/new", O_RDONLY);
    ok = unlink("/tmp/new") == 0 && open("/tmp/new", O_RDONLY) < 0;
    ok &= read(fd, buf, sizeof(buf)) == 4 && buf[0] == 'a';
    fsinfo(&after);
    ok &= after.free_inodes == before.free_inodes - 1;     /* 还开着，没收回 */
    close(fd);
    fsinfo(&after);
    ok &= after.free_inodes == before.free_inodes && after.free_blocks == before.free_blocks;
    write(1, ok ? "u" : "U", 1);

    ok = mkdir("/tmp/d") == 0 && mkdir("/tmp/d") < 0;
    fd = open("/tmp/d/f", O_WRONLY | O_CREAT);
    close(fd);
    ok &= rmdir("/tmp/d") < 0 && rmdir("/") < 0 && rmdir("/tmp/.") < 0 && unlink("/tmp/d") < 0;
    ok &= mkdir("/nothing/d") < 0 && mkdir("/tmp/abcdefghijklmno") < 0;
    ok &= unlink("/tmp/d/f") == 0 && rmdir("/tmp/d") == 0;
    write(1, ok ? "d" : "D", 1);

    int32_t a = open("/tmp/shared", O_RDWR | O_CREAT), b = open("/tmp/shared", O_RDONLY);
    write(a, "same", 4);
    ok = read(b, buf, sizeof(buf)) == 4 && buf[0] == 's' && buf[3] == 'e';
    close(a);
    close(b);
    ok &= unlink("/tmp/shared") == 0;
    fsinfo(&after);
    ok &= after.free_inodes == before.free_inodes && after.free_blocks == before.free_blocks;
    write(1, ok ? "s" : "S", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_SIGNAL
    /*
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 tikgspwevzhrxbuq。被信号结束的状态是 0x200 加信号编号。
     *   t：kill 一个在 sleep 里的子进程：sleep 被打断，它被 SIGTERM 结束。
     *   i：子进程忽略 SIGTERM：kill 返回 0，它照常睡完、退出。
     *   k：SIGKILL 不能忽略，signal(SIGKILL, …) 返回 SIG_ERR；忽略了 SIGTERM 的子进程照样被 SIGKILL 结束。
     *   g：kill(0, …) 发给自己这一组；忽略是 fork 时继承的：改回默认的子进程被结束，没改的照常退出。
     *   s：setsid 之后子进程自成一组：kill(0, …) 找不到它，kill(-它的任务号, …) 才行；第二次 setsid 返回 -1。
     *   p：睡在管道上的读者和写者都能被信号结束。
     *   w：睡在 waitpid 里的子进程能被信号结束，它的子进程成了孤儿，照常运行。
     *   e：读的一头全关了还往管道里写：写的进程被 SIGPIPE 结束；忽略了 SIGPIPE 的，write 返回 -1。
     *   v：不存在的任务、内核任务、init 自己、信号编号不对、pid 为 -1，kill 都返回 -1；signal 的编号不对返回 SIG_ERR。
     *   z：给已经结束、还没被收走的子进程发信号：kill 返回 0，退出码不变。
     *   h：处理函数：子进程 kill 自己，处理函数拿到信号编号，kill 照样返回 0（eax 由 sig_restore 恢复），
     *      局部变量不受影响；第二次还是调用处理函数；signal 返回原来的处理函数。
     *   r：睡在管道上的子进程，信号设了处理函数：处理函数运行，read 返回 -EINTR，程序接着往下走。
     *   x：exec 之后，设过处理函数的信号改回默认（新程序被 SIGTERM 结束），忽略的保持忽略（只有 SIGKILL 结束得了）。
     *   b：处理函数不在用户自己的内存里，signal 返回 SIG_ERR。
     *   u：一直在用户态空转、不做系统调用的子进程，也能被 kill 结束：信号在定时器中断返回用户态时处理。
     *   q：处理函数打断了一段汇编：eax、ecx、edx 和 DF 都由 sig_restore 原样恢复；处理函数里 DF 是 0。
     */
    int32_t st = 0, ok;
    int32_t c = fork();
    if (c == 0) {
        sleep(100000);
        exit(1);
    }
    sleep(3);
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGTERM);
    write(1, ok ? "t" : "T", 1);

    c = fork();
    if (c == 0) {
        signal(SIGTERM, SIG_IGN);
        sleep(20);
        exit(5);
    }
    sleep(5);
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == 5;
    write(1, ok ? "i" : "I", 1);

    c = fork();
    if (c == 0) {
        if (signal(SIGKILL, SIG_IGN) != SIG_ERR || signal(SIGTERM, SIG_IGN) != SIG_DFL || signal(SIGTERM, SIG_IGN) != SIG_IGN)
            exit(9);
        sleep(100000);
        exit(1);
    }
    sleep(3);
    ok = kill(c, SIGTERM) == 0 && kill(c, SIGKILL) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGKILL);
    write(1, ok ? "k" : "K", 1);

    signal(SIGTERM, SIG_IGN);           /* 子进程继承“忽略” */
    int32_t c1 = fork();
    if (c1 == 0) {
        signal(SIGTERM, SIG_DFL);
        sleep(100000);
        exit(1);
    }
    int32_t c2 = fork();
    if (c2 == 0) {
        sleep(30);
        exit(4);
    }
    signal(SIGTERM, SIG_DFL);
    sleep(3);
    int32_t st2 = 0;
    ok = kill(0, SIGTERM) == 0 && waitpid(c1, &st, 0) == c1 && st == (0x200 | SIGTERM) && waitpid(c2, &st2, 0) == c2 && st2 == 4;
    write(1, ok ? "g" : "G", 1);

    c = fork();
    if (c == 0) {
        if (setsid() != getpid() || setsid() != -1)
            exit(1);
        sleep(100000);
        exit(2);
    }
    sleep(3);
    ok = kill(0, SIGTERM) == -1 && kill(-c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGTERM);
    write(1, ok ? "s" : "S", 1);

    int32_t empty[2], full[2];          /* 两个管道：一个没人写，一个没人读 */
    static char big[8192];
    pipe(empty);
    pipe(full);
    int32_t reader = fork();
    if (reader == 0) {
        char ch;
        read(empty[0], &ch, 1);         /* 管道是空的，写的一头还开着（父进程拿着）：一直等 */
        exit(1);
    }
    int32_t writer = fork();
    if (writer == 0) {
        write(full[1], big, sizeof(big));   /* 读的一头开着（父进程拿着）却没人读，写满 4096 字节就等 */
        exit(1);
    }
    sleep(5);
    ok = kill(reader, SIGINT) == 0 && waitpid(reader, &st, 0) == reader && st == (0x200 | SIGINT);
    ok &= kill(writer, SIGINT) == 0 && waitpid(writer, &st, 0) == writer && st == (0x200 | SIGINT);
    close(empty[0]);
    close(empty[1]);
    close(full[0]);
    close(full[1]);
    write(1, ok ? "p" : "P", 1);

    c = fork();
    if (c == 0) {
        int32_t grandchild = fork();
        if (grandchild == 0) {
            sleep(100000);
            exit(0);
        }
        waitpid(grandchild, 0, 0);      /* 孙进程一直在睡：不被信号打断，就永远等下去 */
        exit(1);
    }
    sleep(3);
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGTERM);
    ok &= kill(0, SIGTERM) == 0;        /* 成了孤儿的孙进程还在 init 这一组：结束它 */
    write(1, ok ? "w" : "W", 1);

    int32_t dead[2];
    pipe(dead);
    close(dead[0]);
    c = fork();
    if (c == 0) {
        write(dead[1], "x", 1);
        exit(1);
    }
    ok = waitpid(c, &st, 0) == c && st == (0x200 | SIGPIPE);
    c = fork();
    if (c == 0) {
        signal(SIGPIPE, SIG_IGN);
        exit(write(dead[1], "x", 1) == -1 ? 3 : 1);
    }
    ok &= waitpid(c, &st, 0) == c && st == 3;
    close(dead[1]);
    write(1, ok ? "e" : "E", 1);

    ok = kill(99, SIGTERM) == -1 && kill(1, SIGTERM) == -1 && kill(getpid(), SIGTERM) == -1 && kill(-1, SIGTERM) == -1;
    c = fork();
    if (c == 0) {
        sleep(100000);
        exit(1);
    }
    ok &= kill(c, 0) == -1 && kill(c, 33) == -1 && signal(0, SIG_IGN) == SIG_ERR && signal(33, SIG_IGN) == SIG_ERR;
    ok &= kill(c, SIGKILL) == 0 && waitpid(c, &st, 0) == c;
    write(1, ok ? "v" : "V", 1);

    c = fork();
    if (c == 0)
        exit(6);
    sleep(3);                           /* 子进程已经结束，成了僵尸 */
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == 6;
    write(1, ok ? "z" : "Z", 1);

    caught = caught_count = 0;
    c = fork();
    if (c == 0) {
        volatile int32_t keep = 12345;
        int good = signal(SIGTERM, on_signal) == SIG_DFL;
        good &= kill(getpid(), SIGTERM) == 0 && caught == SIGTERM && caught_count == 1;
        good &= kill(getpid(), SIGTERM) == 0 && caught_count == 2;
        good &= keep == 12345 && signal(SIGTERM, SIG_DFL) == on_signal;
        exit(good ? 0 : 1);
    }
    ok = waitpid(c, &st, 0) == c && st == 0;
    write(1, ok ? "h" : "H", 1);

    int32_t q[2];
    pipe(q);
    caught = 0;
    c = fork();
    if (c == 0) {
        signal(SIGTERM, on_signal);
        char ch;
        int32_t r = read(q[0], &ch, 1);     /* 写的一头开着、没人写：一直等，直到信号来 */
        exit(r == -EINTR && caught == SIGTERM ? 0 : 1);
    }
    sleep(3);
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == 0;
    write(1, ok ? "r" : "R", 1);

    c = fork();
    if (c == 0) {
        signal(SIGTERM, on_signal);
        if (q[0] != 0) {                    /* cat 从这个管道读：没人写，一直等。init 只开着 1 号，q[0] 多半就是 0 */
            close(0);
            dup(q[0]);
        }
        char *argv[] = { "cat", 0 };
        execv("/bin/cat", argv);
        exit(1);
    }
    c2 = fork();
    if (c2 == 0) {
        signal(SIGTERM, SIG_IGN);
        if (q[0] != 0) {
            close(0);
            dup(q[0]);
        }
        char *argv[] = { "cat", 0 };
        execv("/bin/cat", argv);
        exit(1);
    }
    for (int k = 0; k < 100 && !(is_cat(c) && is_cat(c2)); k++)
        sleep(1);                           /* 等两个子进程都换成 cat */
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGTERM);
    ok &= kill(c2, SIGTERM) == 0;
    sleep(3);                               /* 让 c2 有机会处理 SIGTERM：忽略的话它还活着 */
    ok &= waitpid(c2, &st, WNOHANG) == 0;
    ok &= kill(c2, SIGKILL) == 0 && waitpid(c2, &st, 0) == c2 && st == (0x200 | SIGKILL);
    close(q[0]);
    close(q[1]);
    write(1, ok ? "x" : "X", 1);

    ok = signal(SIGTERM, (sighandler_t)0x00100000) == SIG_ERR;     /* 内核的地址 */
    ok &= signal(SIGTERM, (sighandler_t)0x50000000) == SIG_ERR;    /* 没有映射的地址 */
    write(1, ok ? "b" : "B", 1);

    c = fork();
    if (c == 0)
        for (;;)
            __asm__ volatile("");       /* 不做系统调用：只有定时器中断会把它带进内核 */
    sleep(3);
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGTERM);
    write(1, ok ? "u" : "U", 1);

    caught_count = 0;
    c = fork();
    if (c == 0) {
        signal(SIGTERM, on_signal);
        uint32_t a, cx, dx, flags;
        __asm__ volatile("movl $0x11111111, %%eax\n"
                         "movl $0x22222222, %%ecx\n"
                         "movl $0x33333333, %%edx\n"
                         "std\n"
                         "1: cmpl $0, %4\n"         /* 等处理函数把 caught_count 加一 */
                         "je 1b\n"
                         "pushfl\n"
                         "popl %3\n"
                         "cld\n"
                         : "=a"(a), "=c"(cx), "=d"(dx), "=r"(flags)
                         : "m"(caught_count)
                         : "memory", "cc");
        exit(a == 0x11111111 && cx == 0x22222222 && dx == 0x33333333 && (flags & 0x400) && !(handler_flags & 0x400) ? 0 : 1);
    }
    sleep(3);
    ok = kill(c, SIGTERM) == 0 && waitpid(c, &st, 0) == c && st == 0;
    write(1, ok ? "q" : "Q", 1);          /* 不用 a：测试会把第 6 行里任务 A 写的 A 都去掉 */
    sleep(5);                           /* 等孤儿处理完信号、退出，第 7 行最后留下的是 init 的消息 */
    exit(0);
#elif SELFTEST == SELFTEST_USER_BRK
    /*
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 qhgfleoksnm：
     *   q：init 的代码在内核映像里，用户空间里还什么都没有：堆从 0x40000000 开始，brk(0) 返回它。
     *   h：brk 往上挪 8 KB，返回新的末尾；写满这 8 KB 再读回来：两页都是用到时缺页补上的。
     *   g：堆末尾往上、栈区以下没要过的地方，子进程一写就被缺页异常结束；brk 不对齐到页时，它所在的那一页能用。
     *   f：fork 之后子进程的堆和父进程一样大、内容一样；子进程改了不影响父进程（写时复制）。
     *   l：低于程序末尾、离栈区不到 16 KB 的 brk 不动，返回原来的末尾；栈区往下 16 KB 那里还能 brk 到；能缩回空堆。
     *   e：系统调用的缓冲区在堆以外没要过的地方，或者跨过堆的末尾：read 返回 -1。
     *   o：exec 之后新程序的堆从它自己的 .bss 末尾开始，malloc 来的 100 KB 能用（check brk 的退出码）。
     *   k：栈区是顶端的 64 KB：最低的那个字节能用，再往下一个字节就是空地；想把堆推到紧贴栈区，推不上去，
     *      栈用超了碰到的是空地，进程被结束，而不是悄悄写进堆里。
     *   s：sort 按字节排序，高位字节（0x80、0xFF）排在后面，行里可以有 0；-r 倒过来。
     *   n：sort 读两个文件，前一个的最后一行没有换行，也不和后一个的第一行接在一起。
     *   m：malloc 的检查（check malloc 的退出码）：空块合并、和 sbrk 混用、realloc、随机分配和释放。
     */
    uint32_t base = brk(0);
    write(1, base == 0x40000000 ? "q" : "Q", 1);

    uint32_t top = brk(base + 8192);
    int ok = top == base + 8192;
    for (uint32_t a = base; ok && a < top; a += 4)
        *(volatile uint32_t *)a = a;
    for (uint32_t a = base; ok && a < top; a += 4)
        ok &= *(volatile uint32_t *)a == a;
    write(1, ok ? "h" : "H", 1);

    int32_t st = 0;
    int32_t c = fork();
    if (c == 0) {
        *(volatile uint32_t *)(top + 100) = 1;      /* 堆末尾往上那一页还没要过 */
        exit(1);
    }
    ok = waitpid(c, &st, 0) == c && st == (0x100 | 14);
    brk(top + 100);                     /* 不对齐到页：top + 100 所在的整页都算堆 */
    c = fork();
    if (c == 0) {
        *(volatile uint32_t *)(top + 2000) = 5;
        exit(*(volatile uint32_t *)(top + 2000) == 5 ? 0 : 1);
    }
    ok &= waitpid(c, &st, 0) == c && st == 0;
    ok &= brk(top) == top;
    write(1, ok ? "g" : "G", 1);

    *(volatile uint32_t *)base = 42;
    c = fork();
    if (c == 0) {
        int good = brk(0) == top && *(volatile uint32_t *)base == 42;
        *(volatile uint32_t *)base = 7;
        exit(good ? 0 : 1);
    }
    ok = waitpid(c, &st, 0) == c && st == 0 && *(volatile uint32_t *)base == 42;
    write(1, ok ? "f" : "F", 1);

    ok = brk(base - 4) == top && brk(0x403EC004) == top && brk(0x403F0000) == top && brk(0x403EC000) == 0x403EC000;
    ok &= brk(base) == base && brk(top) == top;     /* 缩回空堆，再挪回来 */
    write(1, ok ? "l" : "L", 1);

    int32_t fd = open("/hello.txt", 0);
    ok = read(fd, (void *)(top + 0x100000), 4) == -1 && read(fd, (void *)(top - 2), 4) == -1 && read(fd, (void *)base, 4) == 4;
    close(fd);
    write(1, ok ? "e" : "E", 1);

    c = fork();
    if (c == 0) {
        char *argv[] = { "check", "brk", 0 };
        execv("/bin/check", argv);
        exit(9);
    }
    ok = waitpid(c, &st, 0) == c && st == 0;
    write(1, ok ? "o" : "O", 1);

    c = fork();
    if (c == 0) {
        *(volatile char *)0x403F0000 = 1;           /* 栈区最低的那个字节 */
        exit(*(volatile char *)0x403F0000 == 1 ? 0 : 1);
    }
    ok = waitpid(c, &st, 0) == c && st == 0;
    c = fork();
    if (c == 0) {
        *(volatile char *)0x403EFFFF = 1;           /* 再往下一个字节：空地 */
        exit(1);
    }
    ok &= waitpid(c, &st, 0) == c && st == (0x100 | 14);
    c = fork();
    if (c == 0) {
        brk(0x403F0000);                            /* 推不上去：最高到 0x403EC000 */
        /* 栈一下子用掉 70 KB，伸出了栈区。用汇编挪 ESP：clang 会把只碰一个元素的大数组优化掉 */
        __asm__ volatile("subl $0x11800, %%esp\n"
                         "movb $1, (%%esp)\n"
                         "addl $0x11800, %%esp" ::: "memory");
        exit(1);
    }
    ok &= waitpid(c, &st, 0) == c && st == (0x100 | 14);
    write(1, ok ? "k" : "K", 1);

    static const char in[] = "b\n\x80x\na\0z\na\n\xff\n\nab\n";
    static const char want[] = "\na\na\0z\nab\nb\n\x80x\n\xff\n";
    static const char want_r[] = "\xff\n\x80x\nb\nab\na\0z\na\n\n";
    static char out[64];
    char *plain[] = { "sort", 0 }, *rev[] = { "sort", "-r", 0 };
    int32_t n = run_sort(plain, in, sizeof(in) - 1, out, sizeof(out));
    ok = n == (int32_t)sizeof(want) - 1 && same(out, want, n);
    n = run_sort(rev, in, sizeof(in) - 1, out, sizeof(out));
    ok &= n == (int32_t)sizeof(want_r) - 1 && same(out, want_r, n);
    write(1, ok ? "s" : "S", 1);

    put_file("/tmp/s1", "zzz", 3);                  /* 最后一行没有换行 */
    put_file("/tmp/s2", "aaa\nmmm", 7);
    char *two[] = { "sort", "/tmp/s1", "/tmp/s2", 0 };
    n = run_sort(two, "", 0, out, sizeof(out));
    ok = n == 12 && same(out, "aaa\nmmm\nzzz\n", 12);
    unlink("/tmp/s1");
    unlink("/tmp/s2");
    write(1, ok ? "n" : "N", 1);

    c = fork();
    if (c == 0) {
        char *argv[] = { "check", "malloc", 0 };
        execv("/bin/check", argv);
        exit(9);
    }
    ok = waitpid(c, &st, 0) == c && st == 0;
    write(1, ok ? "m" : "M", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_ALARM
    /*
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 crpkfe：
     *   c：alarm(1) 之后 sleep 3 秒：1 秒后 SIGALRM 来了，处理函数接住，正在睡的 sleep 被打断，提前返回
     *      （按 time 算睡了 1 或 2 秒；不被打断要睡满 3 秒）。
     *   r：alarm 返回上一个闹钟还剩几秒（向上取整）：没有就是 0，刚定了 3 秒就是 3，过了半秒还是 3；alarm(0) 取消，之后睡 1.5 秒也不响。
     *   p：pause 睡到闹钟响，处理函数运行之后返回 -EINTR。
     *   k：没设处理函数的子进程，闹钟一响就被 SIGALRM 结束（状态 0x20E）。
     *   f：fork 出的子进程没有父进程的闹钟。
     *   e：exec 之后闹钟还在：子进程定了闹钟再换成 cat（在没人写的管道上等），1 秒后被 SIGALRM 结束。
     * 整个检查要好几秒，第 6 行早被任务 A 的 A 绕回来盖过了：结果先攒着，最后一次写出去。
     */
    char res[8];
    int nres = 0;
    int32_t st = 0, c;
    signal(SIGALRM, on_alarm);
    rang = 0;
    alarm(1);
    uint32_t t0 = time();
    sleep(300);
    uint32_t slept = time() - t0;
    int ok = rang == SIGALRM && slept >= 1 && slept <= 2;
    res[nres++] = ok ? 'c' : 'C';

    rang = 0;
    ok = alarm(3) == 0 && alarm(3) == 3;
    sleep(50);                              /* 还剩 2.5 秒左右：向上取整是 3 */
    ok &= alarm(0) == 3 && alarm(0) == 0;
    sleep(150);
    ok &= rang == 0;
    res[nres++] = ok ? 'r' : 'R';

    alarm(1);
    ok = pause() == -EINTR && rang == SIGALRM;
    res[nres++] = ok ? 'p' : 'P';

    c = fork();
    if (c == 0) {
        signal(SIGALRM, SIG_DFL);
        alarm(1);
        pause();
        exit(1);
    }
    ok = waitpid(c, &st, 0) == c && st == (0x200 | SIGALRM);
    res[nres++] = ok ? 'k' : 'K';

    alarm(5);
    c = fork();
    if (c == 0)
        exit(alarm(0) == 0 ? 0 : 1);
    ok = waitpid(c, &st, 0) == c && st == 0 && alarm(0) >= 4;
    res[nres++] = ok ? 'f' : 'F';

    int32_t q[2];
    pipe(q);
    c = fork();
    if (c == 0) {
        signal(SIGALRM, SIG_DFL);
        alarm(1);
        if (q[0] != 0) {
            close(0);
            dup(q[0]);
        }
        char *argv[] = { "cat", 0 };
        execv("/bin/cat", argv);
        exit(9);
    }
    ok = waitpid(c, &st, 0) == c && st == (0x200 | SIGALRM);
    close(q[0]);
    close(q[1]);
    res[nres++] = ok ? 'e' : 'E';
    write(1, res, (uint32_t)nres);
    exit(0);
#elif SELFTEST == SELFTEST_USER_SYNC
    /*
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 dsewr（fsinfo 报告脏块数和开机以来写回的块数）：
     *   d：新建一个 3 KB 的文件：缓冲区里多了几块脏的，硬盘上一块也没写。
     *   s：sync 返回 0：脏块都写回了，写回的块数正好多了刚才那么多；再 sync 一次，一块也不用写。
     *   e：写一个 40 KB 的文件：比 16 个缓冲区还多，换块时脏块先写回硬盘，没 sync 也写了不少，脏的最多 16 块。
     *   w：从头读这个文件，逐字节核对：前面的块早被换出去了，是从硬盘读回来的，写回的内容没错。
     *   r：删掉两个文件再 sync：块和 i 节点都收回，没有脏块，df 的数字回到开始时的样子。
     */
    struct fsinfo a, b, c, d;
    static char buf[512];
    fsinfo(&a);
    int32_t fd = open("/tmp/f", O_WRONLY | O_CREAT);
    for (int i = 0; i < 6; i++)
        write(fd, buf, sizeof(buf));
    close(fd);
    fsinfo(&b);
    int ok = a.dirty == 0 && b.dirty > 0 && b.written == a.written;
    write(1, ok ? "d" : "D", 1);

    ok = sync() == 0;
    fsinfo(&c);
    ok &= c.dirty == 0 && c.written == b.written + b.dirty;
    sync();
    fsinfo(&d);
    ok &= d.written == c.written;
    write(1, ok ? "s" : "S", 1);

    fd = open("/tmp/g", O_WRONLY | O_CREAT);
    for (int k = 0; k < 80; k++) {
        for (int i = 0; i < 512; i++)
            buf[i] = (char)(k * 7 + i);
        write(fd, buf, sizeof(buf));
    }
    close(fd);
    fsinfo(&b);
    ok = b.written > d.written + 16 && b.dirty <= b.buffers;
    write(1, ok ? "e" : "E", 1);

    fd = open("/tmp/g", O_RDONLY);
    ok = 1;
    for (int k = 0; k < 80; k++) {
        ok &= read(fd, buf, sizeof(buf)) == 512;
        for (int i = 0; i < 512; i++)
            ok &= buf[i] == (char)(k * 7 + i);
    }
    ok &= read(fd, buf, sizeof(buf)) == 0;
    close(fd);
    write(1, ok ? "w" : "W", 1);

    ok = unlink("/tmp/f") == 0 && unlink("/tmp/g") == 0 && sync() == 0;
    fsinfo(&c);
    ok &= c.dirty == 0 && c.free_blocks == a.free_blocks && c.free_inodes == a.free_inodes;
    write(1, ok ? "r" : "R", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_LINK
    /*
     * 每一项通过就记一个字母，期望第 6 行（去掉任务 A 的 A）是 lwuoemtv：
     *   l：link 之后两个名字的 i 节点号一样，链接数是 2，从新名字读到的就是原来的内容。
     *   w：从一个名字写进去，从另一个名字马上读得到：内存里是同一个 i 节点。
     *   u：删掉原来的名字，另一个名字照常能读，链接数变回 1，块和 i 节点都没收回；再删掉它，才都收回。
     *   o：两个名字都删了，可文件还开着：照常能读出原来的内容；关掉以后才收回。
     *   e：目录不能 link；新名字已经存在（包括 . 和 ..）、源文件不存在、新名字所在的目录不存在，都返回 -1。
     *   m：一个文件最多 250 个名字：再 link 返回 -1，链接数还是 250（一个字节，再加就回绕成 0，文件被收回）。
     *   t：以 / 结尾的路径说的是目录：文件后面加 / 打不开、stat 不到、删不掉，也不能用它新建文件或链接；
     *      mkdir、stat、rmdir 目录时末尾带 / 照常可以。
     *   v：mv 搬进目录时拼出来的名字超过 127 个字节：报错退出，不截短（截短的名字正好是目录里另一个文件，会把它删掉）。
     * 要运行 /bin/mv，后面几项比较慢，第 6 行会被任务 A 的 A 绕回来盖掉：结果先攒着，最后一次写出去。
     */
    struct fsinfo before, now;
    struct stat a, b;
    char buf[16], res[8];
    int nres = 0;
    fsinfo(&before);
    int32_t fd = open("/tmp/a", O_WRONLY | O_CREAT);
    write(fd, "hello", 5);
    close(fd);
    int ok = link("/tmp/a", "/tmp/b") == 0 && stat("/tmp/a", &a) == 0 && stat("/tmp/b", &b) == 0;
    ok &= a.ino == b.ino && a.nlinks == 2 && b.nlinks == 2;
    fd = open("/tmp/b", O_RDONLY);
    ok &= read(fd, buf, sizeof(buf)) == 5 && buf[0] == 'h' && buf[4] == 'o';
    close(fd);
    res[nres++] = ok ? 'l' : 'L';

    int32_t wfd = open("/tmp/a", O_WRONLY | O_APPEND), rfd = open("/tmp/b", O_RDONLY);
    write(wfd, "!", 1);
    ok = read(rfd, buf, sizeof(buf)) == 6 && buf[5] == '!';
    close(wfd);
    close(rfd);
    res[nres++] = ok ? 'w' : 'W';

    ok = unlink("/tmp/a") == 0 && stat("/tmp/a", &a) < 0 && stat("/tmp/b", &b) == 0 && b.nlinks == 1;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes - 1 && now.free_blocks == before.free_blocks - 1;
    ok &= unlink("/tmp/b") == 0;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes && now.free_blocks == before.free_blocks;
    res[nres++] = ok ? 'u' : 'U';

    fd = open("/tmp/c", O_WRONLY | O_CREAT);
    write(fd, "xyz", 3);
    rfd = open("/tmp/c", O_RDONLY);                     /* 没有 lseek：另开一个从头读的描述符 */
    ok = link("/tmp/c", "/tmp/d") == 0 && unlink("/tmp/c") == 0 && unlink("/tmp/d") == 0;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes - 1;    /* 还开着：没收回 */
    ok &= open("/tmp/c", O_RDONLY) < 0 && open("/tmp/d", O_RDONLY) < 0;
    ok &= read(rfd, buf, sizeof(buf)) == 3 && buf[0] == 'x' && buf[2] == 'z';
    close(fd);
    close(rfd);
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes && now.free_blocks == before.free_blocks;
    res[nres++] = ok ? 'o' : 'O';

    fd = open("/tmp/e", O_WRONLY | O_CREAT);
    close(fd);
    ok = link("/tmp", "/tmp/dir2") < 0 && link("/tmp/e", "/tmp/e") < 0 && link("/tmp/e", "/tmp/.") < 0;
    ok &= link("/tmp/e", "/tmp/..") < 0;
    ok &= link("/nothing", "/tmp/f") < 0 && link("/tmp/e", "/nothing/f") < 0 && link("/tmp/e", "/tmp/abcdefghijklmno") < 0;
    ok &= stat("/tmp/e", &a) == 0 && a.nlinks == 1 && unlink("/tmp/e") == 0;
    res[nres++] = ok ? 'e' : 'E';

    /* 249 个新名字放在单独的目录里：一项 16 字节，4 块。数组不用字符串初始化：clang 会调用内核里的 memset，用户态执行不了 */
    char name[12];
    for (int i = 0; i < 8; i++)
        name[i] = "/tmp/ml/"[i];
    name[11] = '\0';
    ok = mkdir("/tmp/ml") == 0 && (fd = open("/tmp/m", O_WRONLY | O_CREAT)) >= 0;
    close(fd);
    for (int i = 0; i < 249 && ok; i++) {
        name[8] = (char)('0' + i / 100), name[9] = (char)('0' + i / 10 % 10), name[10] = (char)('0' + i % 10);
        ok = link("/tmp/m", name) == 0;
    }
    ok &= stat("/tmp/m", &a) == 0 && a.nlinks == 250;
    ok &= link("/tmp/m", "/tmp/ml/more") < 0 && stat("/tmp/ml/more", &b) < 0;
    ok &= stat("/tmp/m", &a) == 0 && a.nlinks == 250;
    for (int i = 0; i < 249; i++) {
        name[8] = (char)('0' + i / 100), name[9] = (char)('0' + i / 10 % 10), name[10] = (char)('0' + i % 10);
        ok &= unlink(name) == 0;
    }
    ok &= stat("/tmp/m", &a) == 0 && a.nlinks == 1 && unlink("/tmp/m") == 0 && rmdir("/tmp/ml") == 0;
    res[nres++] = ok ? 'm' : 'M';

    ok = open("/tmp/t/", O_WRONLY | O_CREAT) < 0 && stat("/tmp/t", &a) < 0;
    fd = open("/tmp/t", O_WRONLY | O_CREAT);
    close(fd);
    ok &= fd >= 0 && stat("/tmp/t/", &a) < 0 && open("/tmp/t/", O_RDONLY) < 0 && unlink("/tmp/t/") < 0;
    ok &= link("/tmp/t", "/tmp/u/") < 0 && stat("/tmp/u", &a) < 0 && link("/tmp/t/", "/tmp/u") < 0;
    ok &= mkdir("/tmp/dd/") == 0 && stat("/tmp/dd/", &a) == 0 && (a.mode & 0170000) == 0040000 && rmdir("/tmp/dd/") == 0;
    ok &= stat("/tmp/t", &a) == 0 && unlink("/tmp/t") == 0;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes && now.free_blocks == before.free_blocks;
    res[nres++] = ok ? 't' : 'T';

    /* 120 个字节的目录：/tmp/ 加 7 段 14 个字母和 1 段 10 个字母；它下面的 victim 正好 127 个字节 */
    char dir[128], victim[128];
    int n = 4;
    for (int i = 0; i < n; i++)
        dir[i] = "/tmp"[i];
    ok = 1;
    for (int k = 0; k < 8; k++) {
        dir[n++] = '/';
        for (int i = 0; i < (k < 7 ? 14 : 10); i++)
            dir[n++] = (char)('a' + k);
        dir[n] = '\0';
        ok &= mkdir(dir) == 0;
    }
    for (int i = 0; i <= n; i++)
        victim[i] = dir[i];
    const char *v = "/victim";
    for (int i = 0; v[i]; i++)
        victim[n + i] = v[i];
    victim[n + 7] = '\0';
    ok &= n == 120;
    fd = open(victim, O_WRONLY | O_CREAT);
    ok &= write(fd, "keep me", 7) == 7;
    close(fd);
    fd = open("/tmp/victimXYZ", O_WRONLY | O_CREAT);
    ok &= write(fd, "moved in", 8) == 8;
    close(fd);
    char *mv_argv[] = { "mv", "/tmp/victimXYZ", dir, 0 };
    int32_t st = -1, pid = fork();
    if (pid == 0) {
        execv("/bin/mv", mv_argv);
        exit(99);
    }
    waitpid(pid, &st, 0);
    ok &= st == 1;
    fd = open(victim, O_RDONLY);
    ok &= read(fd, buf, sizeof(buf)) == 7 && buf[0] == 'k';
    close(fd);
    ok &= unlink(victim) == 0 && unlink("/tmp/victimXYZ") == 0;
    for (int k = 7; k >= 0; k--) {
        ok &= rmdir(dir) == 0;
        while (n > 0 && dir[n] != '/')
            n--;
        dir[n] = '\0';
    }
    res[nres++] = ok ? 'v' : 'V';
    write(1, res, (uint32_t)nres);
    exit(0);
#elif SELFTEST == SELFTEST_USER_TIME
    /*
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 ctmdu：
     *   c：日期换算：1970-01-01 是 0；2000-02-29 12:00 是 951825600（2000 年能被 400 整除，是闰年）；
     *      2100-03-01 是 4107542400，它前一秒是 2100-02-28 23:59:59（2100 年不是闰年）；0xFFFFFFFF 是 2106-02-07 06:28:15。
     *   t：time 是开机时从 CMOS 读的时钟往后数：年份在 2024 以后；睡 1.5 秒，time 多了 1 或 2。
     *   m：新建的文件，修改时间就是现在；utime 改成 12345，stat 读回来就是 12345；往里写东西，又变成现在。
     *   d：目录里加一项、删一项，目录的修改时间也变成现在。
     *   u：文件的主人和组都是 0（root）。
     * 中间要睡 1.5 秒，第 6 行会被任务 A 的 A 绕回来盖掉：结果先攒着，最后一次写出去。
     */
    char res[8];
    int nres = 0;
    struct date d = { 1970, 1, 1, 0, 0, 0 };
    int ok = date_to_unix(&d) == 0;
    d = (struct date){ 2000, 2, 29, 12, 0, 0 };
    ok &= date_to_unix(&d) == 951825600;
    d = (struct date){ 2100, 3, 1, 0, 0, 0 };
    ok &= date_to_unix(&d) == 4107542400u;
    unix_to_date(4107542400u - 1, &d);
    ok &= d.year == 2100 && d.month == 2 && d.day == 28 && d.hour == 23 && d.minute == 59 && d.second == 59;
    unix_to_date(0xFFFFFFFFu, &d);
    ok &= d.year == 2106 && d.month == 2 && d.day == 7 && d.hour == 6 && d.minute == 28 && d.second == 15;
    unix_to_date(951825600, &d);
    ok &= d.year == 2000 && d.month == 2 && d.day == 29 && d.hour == 12;
    res[nres++] = ok ? 'c' : 'C';

    uint32_t t1 = time();
    unix_to_date(t1, &d);
    sleep(150);
    uint32_t t2 = time();
    ok = d.year >= 2024 && t2 - t1 >= 1 && t2 - t1 <= 2;
    res[nres++] = ok ? 't' : 'T';

    struct stat st;
    int32_t fd = open("/tmp/f", O_WRONLY | O_CREAT);
    uint32_t now = time();
    ok = fd >= 0 && stat("/tmp/f", &st) == 0 && st.mtime + 1 >= now && st.mtime <= now;
    ok &= utime("/tmp/f", 12345) == 0 && stat("/tmp/f", &st) == 0 && st.mtime == 12345;
    write(fd, "x", 1);
    close(fd);
    now = time();
    ok &= stat("/tmp/f", &st) == 0 && st.mtime + 1 >= now && st.mtime <= now;
    ok &= utime("/tmp/f", 0) == 0 && stat("/tmp/f", &st) == 0 && st.mtime + 1 >= now;
    ok &= utime("/nothing", 0) < 0;
    res[nres++] = ok ? 'm' : 'M';

    ok = utime("/tmp", 1000) == 0 && unlink("/tmp/f") == 0;
    now = time();
    ok &= stat("/tmp", &st) == 0 && st.mtime + 1 >= now;
    ok &= utime("/tmp", 1000) == 0 && (fd = open("/tmp/g", O_WRONLY | O_CREAT)) >= 0;
    close(fd);
    ok &= stat("/tmp", &st) == 0 && st.mtime + 1 >= now && unlink("/tmp/g") == 0;
    res[nres++] = ok ? 'd' : 'D';

    ok = stat("/hello.txt", &st) == 0 && st.uid == 0 && st.gid == 0;
    res[nres++] = ok ? 'u' : 'U';
    write(1, res, (uint32_t)nres);
    exit(0);
#elif SELFTEST == SELFTEST_USER_FCNTL
    /*
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 dfcke：
     *   d：dup2 让新描述符指向同一个打开的文件、共用读写位置；dup2(fd, fd) 什么也不做；
     *      新描述符原来开着的先关掉：它是管道写的一头的话，读的一头就读到末尾了。
     *   f：F_DUPFD 找不小于 arg 的最小空闲描述符；F_GETFL 取打开方式；F_SETFL 加上 O_APPEND，以后写的都接在末尾。
     *   c：close-on-exec：F_SETFD 设上，F_GETFD 读得回来，dup 出来的没有；子进程给管道写的一头设上再 exec sleep，
     *      exec 就把它关了：父进程马上读到末尾，那时 sleep 还在睡。
     *   k：fork 出的子进程继承 close-on-exec。
     *   e：不存在的描述符、不认识的命令都返回 -1。
     */
    char buf[16];
    int32_t st = 0;
    int32_t fd = open("/hello.txt", O_RDONLY);
    int ok = dup2(fd, 5) == 5 && read(fd, buf, 6) == 6 && read(5, buf, 4) == 4 && buf[0] == 'f' && buf[3] == 'm';
    ok &= dup2(fd, fd) == fd && dup2(fd, 8) == -1 && dup2(7, 6) == -1;
    close(5);
    int32_t p[2];
    pipe(p);
    ok &= dup2(fd, p[1]) == p[1] && read(p[0], buf, 1) == 0;    /* 写的一头被换成了 hello.txt：没人写了 */
    close(p[0]);
    close(p[1]);
    close(fd);
    write(1, ok ? "d" : "D", 1);

    fd = open("/hello.txt", O_RDONLY);
    ok = fcntl(fd, F_DUPFD, 4) == 4 && fcntl(fd, F_DUPFD, 4) == 5 && fcntl(fd, F_GETFL, 0) == O_RDONLY;
    close(4);
    close(5);
    close(fd);
    int32_t w = open("/tmp/f", O_WRONLY | O_CREAT | O_TRUNC);
    write(w, "abc", 3);
    int32_t again = open("/tmp/f", O_WRONLY);
    ok &= fcntl(again, F_GETFL, 0) == O_WRONLY && fcntl(again, F_SETFL, O_APPEND) == 0 && fcntl(again, F_GETFL, 0) == (O_WRONLY | O_APPEND);
    write(again, "d", 1);                   /* 接在末尾，不是盖掉第一个字节 */
    close(again);
    close(w);
    fd = open("/tmp/f", O_RDONLY);
    ok &= read(fd, buf, sizeof(buf)) == 4 && buf[0] == 'a' && buf[3] == 'd';
    close(fd);
    unlink("/tmp/f");
    write(1, ok ? "f" : "F", 1);

    fd = open("/hello.txt", O_RDONLY);
    ok = fcntl(fd, F_GETFD, 0) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD, 0) == 1;
    int32_t copy = dup(fd);
    ok &= fcntl(copy, F_GETFD, 0) == 0;
    close(copy);
    close(fd);
    pipe(p);
    int32_t c = fork();
    if (c == 0) {
        close(p[0]);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
        char *argv[] = { "sleep", "2", 0 };
        execv("/bin/sleep", argv);
        exit(9);
    }
    close(p[1]);
    ok &= read(p[0], buf, 1) == 0 && waitpid(c, &st, WNOHANG) == 0;    /* 读到末尾时 sleep 还在睡 */
    close(p[0]);
    kill(c, SIGKILL);
    waitpid(c, &st, 0);
    write(1, ok ? "c" : "C", 1);

    fd = open("/hello.txt", O_RDONLY);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    c = fork();
    if (c == 0)
        exit(fcntl(fd, F_GETFD, 0) == 1 ? 0 : 1);
    ok = waitpid(c, &st, 0) == c && st == 0;
    close(fd);
    write(1, ok ? "k" : "K", 1);

    ok = fcntl(7, F_GETFD, 0) == -1 && fcntl(99, F_GETFL, 0) == -1;
    fd = open("/hello.txt", O_RDONLY);
    ok &= fcntl(fd, 99, 0) == -1 && fcntl(fd, F_DUPFD, 8) == -1;
    close(fd);
    write(1, ok ? "e" : "E", 1);
    exit(0);
#elif SELFTEST == SELFTEST_USER_NICE
    /*
     * 每一项通过就记一个字母，期望第 6 行（去掉任务 A 的 A）是 nidfts：
     *   n：nice 返回新的 nice 值：nice(0) 是 0，加 5 是 5，减 3 是 2，加 100 停在 19，减 100 停在 -20；
     *      taskinfo 读到的 nice 和 priority（20 - nice）对得上。
     *   i：fork 出的子进程继承 nice 值；exec 之后也不变（nice 7 时 fork 出的子进程换成 sleep，taskinfo 看到 nice 7、priority 13）。
     *   d：两个一直空转的子进程，一个 nice 0、一个 nice 19，抢 2 秒 CPU：nice 0 的至少分到另一个的 8 倍（按算法是 20 倍），
     *      nice 19 的也没饿死。
     *   f：两个 nice 0 的子进程抢 2 秒：分得差不多（少的至少是多的一半）。
     *   t：用户态空转的子进程，时间几乎都记在 utime；一直调用 stat 的，大半记在 stime。
     *   s：收走的子进程用的时间都加进了父进程的 cutime、cstime，和子进程自己记下的对得上。
     * 整个检查要七八秒：结果先攒着，最后一次写出去。
     */
    char res[8];
    int nres = 0;
    struct taskinfo info;
    int32_t st = -1;
    int ok = nice(0) == 0 && nice(5) == 5 && nice(-3) == 2 && nice(100) == 19 && nice(-100) == -20;
    ok &= taskinfo(getpid(), &info) == 0 && info.nice == -20 && info.priority == 40;
    ok &= nice(20) == 0 && taskinfo(getpid(), &info) == 0 && info.nice == 0 && info.priority == 20;
    res[nres++] = ok ? 'n' : 'N';

    ok = nice(7) == 7;
    int32_t c = fork();
    if (c == 0)
        exit(nice(0) == 7 ? 0 : 1);
    ok &= waitpid(c, &st, 0) == c && st == 0;
    char *sleep_argv[] = { "sleep", "1", 0 };
    c = fork();
    if (c == 0) {
        execv("/bin/sleep", sleep_argv);
        exit(9);
    }
    sleep(30);
    ok &= taskinfo(c, &info) == 0 && info.name[0] == 's' && info.name[1] == 'l' && info.nice == 7 && info.priority == 13;
    ok &= waitpid(c, &st, 0) == c && st == 0 && nice(-7) == 0;
    res[nres++] = ok ? 'i' : 'I';

    struct tms before, after;
    uint32_t sum = 0;
    times(&before);
    uint32_t from = times(0) + 50;
    int32_t p0 = spinner(0, 0, from, from + 200, 0), p1 = spinner(1, 19, from, from + 200, 0);
    waitpid(p0, &st, 0);
    waitpid(p1, &st, 0);
    uint32_t hi = spent_user[0] + spent_sys[0], lo = spent_user[1] + spent_sys[1];
    sum += whole[0] + whole[1];
    ok = hi >= 8 * lo && lo >= 2 && hi + lo >= 150;
    res[nres++] = ok ? 'd' : 'D';

    from = times(0) + 50;
    p0 = spinner(0, 0, from, from + 200, 0);
    p1 = spinner(1, 0, from, from + 200, 0);
    waitpid(p0, &st, 0);
    waitpid(p1, &st, 0);
    uint32_t x = spent_user[0] + spent_sys[0], y = spent_user[1] + spent_sys[1];
    sum += whole[0] + whole[1];
    ok = x + y >= 150 && 2 * (x < y ? x : y) >= (x > y ? x : y);
    res[nres++] = ok ? 'f' : 'F';

    from = times(0) + 5;
    p0 = spinner(0, 0, from, from + 50, 0);
    waitpid(p0, &st, 0);
    from = times(0) + 5;
    p1 = spinner(1, 0, from, from + 50, 1);
    waitpid(p1, &st, 0);
    sum += whole[0] + whole[1];
    ok = spent_user[0] >= 35 && spent_sys[0] * 4 <= spent_user[0] && spent_sys[1] > spent_user[1] && spent_sys[1] >= 25;
    res[nres++] = ok ? 't' : 'T';

    times(&after);
    uint32_t got = (after.tms_cutime - before.tms_cutime) + (after.tms_cstime - before.tms_cstime);
    ok = got >= sum && got <= sum + 10 && after.tms_cutime - before.tms_cutime >= spent_user[0];
    res[nres++] = ok ? 's' : 'S';
    write(1, res, (uint32_t)nres);
    exit(0);
#elif SELFTEST == SELFTEST_USER_STOP
    /*
     * 每一项通过就记一个字母，期望第 6 行（去掉任务 A 的 A）是 swcqkop：
     *   s：子进程给自己发 SIGSTOP 停下：waitpid(WUNTRACED) 返回它，状态是 0x400 加 19；taskinfo 里是停着（6）；
     *      再 waitpid(WUNTRACED | WNOHANG) 返回 0（一次停下只报告一次）；不带 WUNTRACED 的 waitpid 看不见它停下；
     *      SIGCONT 让它接着运行，以 7 退出。
     *   w：给正在睡的进程发 SIGCONT：默认不理，它照常睡醒、退出。
     *   c：SIGSTOP 和 SIGKILL 拦不住（signal 返回 -1）；SIGTSTP 设了处理函数就调用它，不停下；忽略了就什么也不发生。
     *   q：停下的信号让没处理的 SIGCONT 作废，SIGCONT 让没处理的停下的信号作废（停着时发 SIGCONT 不会再停下）。
     *   k：停着的进程收到 SIGKILL 也会被结束，状态是 0x200 加 9。
     *   o：孙子进程停着，它的父进程（子进程）退出了：没人会让它接着运行，内核给它 SIGHUP 和 SIGCONT，它就结束了。
     *   p：setpgid(0, 0) 自己开一个进程组，getpgrp 就是自己的号；不能改别的会话的任务、同一个会话里不是自己孩子的任务（父进程），
     *      也不能进不存在的组。
     * 结果先攒着，最后一次写出去。
     */
    char res[8];
    int nres = 0, ok;
    int32_t st = -1, c;
    struct taskinfo info;

    c = fork();
    if (c == 0) {
        kill(getpid(), SIGSTOP);
        exit(7);
    }
    ok = waitpid(c, &st, WUNTRACED) == c && st == (0x400 | SIGSTOP);
    ok &= taskinfo(c, &info) == 0 && info.state == 6;
    ok &= waitpid(c, &st, WUNTRACED | WNOHANG) == 0 && waitpid(c, &st, WNOHANG) == 0;
    ok &= kill(c, SIGCONT) == 0 && waitpid(c, &st, 0) == c && st == 7;
    res[nres++] = ok ? 's' : 'S';

    c = fork();
    if (c == 0) {
        sleep(30);
        exit(3);
    }
    sleep(5);
    ok = kill(c, SIGCONT) == 0 && waitpid(c, &st, 0) == c && st == 3;
    res[nres++] = ok ? 'w' : 'W';

    ok = signal(SIGSTOP, on_tstp) == (sighandler_t)-1 && signal(SIGKILL, SIG_IGN) == (sighandler_t)-1;
    caught_tstp = 0;
    c = fork();
    if (c == 0) {
        signal(SIGTSTP, on_tstp);
        kill(getpid(), SIGTSTP);
        signal(SIGTSTP, SIG_IGN);
        kill(getpid(), SIGTSTP);
        exit(caught_tstp == SIGTSTP ? 4 : 5);
    }
    ok &= waitpid(c, &st, WUNTRACED) == c && st == 4;
    res[nres++] = ok ? 'c' : 'C';

    c = fork();
    if (c == 0) {
        kill(getpid(), SIGSTOP);
        exit(8);
    }
    ok = waitpid(c, &st, WUNTRACED) == c && st == (0x400 | SIGSTOP);
    ok &= kill(c, SIGTSTP) == 0 && kill(c, SIGCONT) == 0 && waitpid(c, &st, WUNTRACED) == c && st == 8;
    res[nres++] = ok ? 'q' : 'Q';

    c = fork();
    if (c == 0) {
        kill(getpid(), SIGSTOP);
        exit(9);
    }
    ok = waitpid(c, &st, WUNTRACED) == c && kill(c, SIGKILL) == 0 && waitpid(c, &st, 0) == c && st == (0x200 | SIGKILL);
    res[nres++] = ok ? 'k' : 'K';

    grandchild = 0;
    c = fork();
    if (c == 0) {
        int32_t g = fork();
        if (g == 0) {
            kill(getpid(), SIGSTOP);
            exit(0);
        }
        grandchild = g;
        int32_t gs;
        waitpid(g, &gs, WUNTRACED);
        exit(gs == (0x400 | SIGSTOP) ? 0 : 1);  /* 孙子停着，自己退出：它成了孤儿 */
    }
    ok = waitpid(c, &st, 0) == c && st == 0 && grandchild > 0;
    sleep(10);
    ok &= taskinfo(grandchild, &info) < 0;      /* 被 SIGHUP 结束，没有父进程，当场收走了 */
    res[nres++] = ok ? 'o' : 'O';

    c = fork();
    if (c == 0) {
        int good = getpgrp() != getpid() && setpgid(0, 0) == 0 && getpgrp() == getpid();
        good &= setpgid(1, 1) < 0 && setpgid(2, 2) < 0 && setpgid(0, 15) < 0;   /* 2 号是父进程 init，同一个会话但不是孩子 */
        exit(good ? 0 : 1);
    }
    ok = waitpid(c, &st, 0) == c && st == 0;
    res[nres++] = ok ? 'p' : 'P';
    write(1, res, (uint32_t)nres);
    exit(0);
#elif SELFTEST == SELFTEST_USER_SWAP
    /*
     * 内核只留了 300 页空闲物理页（mem.c 的 mem_init），交换区有 1024 页。每一项通过就记一个字母，期望第 6 行是 hkfcxr：
     *   h：子进程把堆挪大 2 MB（512 页，比空闲页还多），每页写上记号，再从头读一遍都对；交换区用上了。
     *   k：最早写的那几页早换出去了：把它们交给 write、read，内核读写换出去的用户内存要先读回来，读写的内容都对。
     *   f：这个子进程再 fork：孙子读到的堆和它一样；孙子改写前一半，它自己的还是原样（换出去的页父子各读回一份）。
     *   c：两个子进程一起各用 1 MB，轮流睡、来回写三遍，每遍核对都对。
     *   x：一个子进程占着 1.5 MB，它的孩子 exec 磁盘上的 check swap：只读的页扔掉以后从程序文件读回来，
     *      写过的 .data、.bss 从交换区读回来（要是当成没写过扔掉，再从文件读就是原来的值）；它自己的 1.5 MB 也都对。
     *   r：都结束以后，交换区一页也没用，空闲的物理页和开始时一样多。
     * 结果先攒着，最后一次写出去。
     */
    char res[8];
    int nres = 0, ok;
    int32_t st = -1, c, d;
    struct fsinfo info;
    ok = fsinfo(&info) == 0 && info.swap_total == 1024 && info.swap_used == 0;
    uint32_t free_before = info.free_pages;

    c = fork();
    if (c == 0) {
        uint32_t base = swap_grow(512);
        int good = base != 0;
        swap_fill(base, 512, 1000);
        good &= swap_check(base, 512, 1000);
        struct fsinfo fi;
        good &= fsinfo(&fi) == 0 && fi.swap_used > 0;

        int32_t fds[2];
        int kgood = pipe(fds) == 0;
        kgood &= write(fds[1], (const void *)base, 8) == 8;             /* 第 0 页的头 8 个字节：1000 和 0 */
        kgood &= read(fds[0], (void *)(base + 4096 + 8), 8) == 8;       /* 读到第 1 页的第 2、3 个字里 */
        volatile uint32_t *p1 = (volatile uint32_t *)(base + 4096);
        kgood &= p1[0] == 1001 && p1[2] == 1000 && p1[3] == 0;
        p1[2] = p1[3] = 0;

        int32_t g = fork();
        if (g == 0) {
            int gg = swap_check(base, 512, 1000);
            swap_fill(base, 256, 5000);
            gg &= swap_check(base, 256, 5000) && swap_check(base + 256 * 4096, 256, 1256);
            exit(gg ? 0 : 1);
        }
        int32_t gs = -1;
        int fgood = g > 0 && waitpid(g, &gs, 0) == g && gs == 0 && swap_check(base, 512, 1000);
        exit((good ? 1 : 0) | (kgood ? 2 : 0) | (fgood ? 4 : 0));
    }
    ok &= waitpid(c, &st, 0) == c;
    res[nres++] = ok && (st & 1) ? 'h' : 'H';
    res[nres++] = ok && (st & 2) ? 'k' : 'K';
    res[nres++] = ok && (st & 4) ? 'f' : 'F';

    c = fork();
    if (c == 0)
        exit(swap_churn(256, 2000) ? 0 : 1);
    d = fork();
    if (d == 0)
        exit(swap_churn(256, 3000) ? 0 : 1);
    ok = waitpid(c, &st, 0) == c && st == 0;
    ok &= waitpid(d, &st, 0) == d && st == 0;
    res[nres++] = ok ? 'c' : 'C';

    c = fork();
    if (c == 0) {
        uint32_t base = swap_grow(384);
        int good = base != 0;
        swap_fill(base, 384, 7000);
        int32_t g = fork();
        if (g == 0) {
            char *argv[] = { "check", "swap", "200", 0 };
            close(1);                   /* 它打印的 swap ok 不要落在第 6 行上：只看退出码 */
            execv("/bin/check", argv);
            exit(9);
        }
        int32_t gs = -1;
        good &= g > 0 && waitpid(g, &gs, 0) == g && gs == 0 && swap_check(base, 384, 7000);
        exit(good ? 0 : 1);
    }
    ok = waitpid(c, &st, 0) == c && st == 0;
    res[nres++] = ok ? 'x' : 'X';

    ok = fsinfo(&info) == 0 && info.swap_used == 0 && info.free_pages == free_before;
    res[nres++] = ok ? 'r' : 'R';
    write(1, res, (uint32_t)nres);
    exit(0);
#elif SELFTEST == SELFTEST_USER_PERM
    /*
     * init 是 root：先准备好几个 root 的文件，再 fork 一个子进程换成用户 100（组 100），由它逐项检查，结果记在 perm_res。
     * 每一项通过记一个小写字母，期望第 6 行（去掉任务 A 的 A）是 urcsxmke：
     *   u：init 的四个号都是 0；子进程 setgid(100)、setuid(100) 以后四个号都是 100，再也 setuid(0)、setgid(0) 不回去。
     *   r：别人能读的能读，不能写的写不了（/etc/passwd、root 的 0644 文件），O_TRUNC 也算写；0700 的目录读不了。
     *   c：/ 和 /etc 是 root 的 0755：在里面建不了文件、目录、链接；在 /tmp 能建，主人和组是 100，root 能删掉它。
     *   s：/tmp 有粘滞位：删不掉 root 放在里面的文件，自己的能删。
     *   x：没有执行权限的程序（0644）执行不了，root 也不行；0755 的能执行；0700 的目录里的文件 stat 不到、进不去，root 可以。
     *   m：别人的文件 chmod 不了，自己的能；谁都不能 chown，只有 root 能，改了主人 setuid 位就清掉；
     *      utime 设成现在要有写权限，设成别的时间要是主人。
     *   k：kill 不了 root 的进程，能 kill 自己的；nice 调低不了（返回 -1，不变），调高可以。
     *   e：setuid 程序：/bin/id 设上 setuid 位（主人是 root），用户 100 运行它，打印 euid=0(root)。
     *   n：mknod：用户 100 建不了设备文件，能建命名管道；root 建的字符设备 4,1（第 6 行）写得进去（写的是 !，见最后）。
     */
    struct stat st;
    int32_t fd, status = -1;
    int ok = getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0;
    fd = open("/tmp/rootfile", O_WRONLY | O_CREAT);
    write(fd, "root data", 9);
    close(fd);
    fd = open("/tmp/rootw", O_WRONLY | O_CREAT);
    close(fd);
    chmod("/tmp/rootw", 0666);
    mkdir("/tmp/priv");
    chmod("/tmp/priv", 0700);
    close(open("/tmp/priv/f", O_WRONLY | O_CREAT));
    ok &= copy_file("/bin/hello", "/tmp/hx") && chmod("/tmp/hx", 0644) == 0;
    int32_t sleeper = fork();
    if (sleeper == 0) {
        sleep(1000);
        exit(0);
    }
    chmod("/bin/id", 04755);
    char out[64];
    uint32_t got;

    int32_t c = fork();
    if (c == 0) {
        ok &= setgid(100) == 0 && setuid(100) == 0;
        ok &= getuid() == 100 && geteuid() == 100 && getgid() == 100 && getegid() == 100;
        ok &= setuid(0) == -1 && setgid(0) == -1 && geteuid() == 100 && getegid() == 100 && setuid(100) == 0;
        perm_put(ok, 'u');

        fd = open("/etc/passwd", O_RDONLY);
        ok = fd >= 0;
        close(fd);
        ok &= open("/etc/passwd", O_WRONLY) < 0 && open("/hello.txt", O_RDWR) < 0 && open("/tmp/rootfile", O_WRONLY) < 0;
        ok &= open("/tmp/rootfile", O_RDONLY | O_TRUNC) < 0 && stat("/tmp/rootfile", &st) == 0 && st.size == 9;
        ok &= open("/tmp/priv", O_RDONLY) < 0;
        perm_put(ok, 'r');

        ok = open("/new", O_WRONLY | O_CREAT) < 0 && mkdir("/newdir") < 0 && stat("/new", &st) < 0;
        fd = open("/tmp/mine", O_WRONLY | O_CREAT);
        ok &= fd >= 0 && write(fd, "mine", 4) == 4;
        close(fd);
        ok &= stat("/tmp/mine", &st) == 0 && st.uid == 100 && st.gid == 100 && st.mode == 0100644;
        ok &= mkdir("/tmp/mydir") == 0 && stat("/tmp/mydir", &st) == 0 && st.uid == 100 && rmdir("/tmp/mydir") == 0;
        ok &= link("/tmp/mine", "/etc/stolen") < 0 && link("/tmp/mine", "/tmp/mine2") == 0;
        perm_put(ok, 'c');

        ok = unlink("/tmp/rootfile") < 0 && stat("/tmp/rootfile", &st) == 0 && unlink("/tmp/mine2") == 0;
        perm_put(ok, 's');

        ok = run_capture("/tmp/hx", out, sizeof(out), &got) == 42;     /* 输出接到管道上：hello 万一运行了也不写第 6 行 */
        ok &= stat("/tmp/priv/f", &st) < 0 && chdir("/tmp/priv") < 0;
        perm_put(ok, 'x');

        ok = chmod("/tmp/rootfile", 0666) < 0 && chmod("/tmp/mine", 0600) == 0 && stat("/tmp/mine", &st) == 0;
        ok &= st.mode == 0100600 && chown("/tmp/mine", 0, 0) < 0 && chown("/tmp/mine", 100, 100) < 0;
        ok &= utime("/tmp/rootfile", 12345) < 0 && utime("/tmp/rootfile", 0) < 0;
        ok &= utime("/tmp/rootw", 0) == 0 && utime("/tmp/rootw", 12345) < 0 && utime("/tmp/mine", 12345) == 0;
        perm_put(ok, 'm');

        int32_t mine = fork();
        if (mine == 0) {
            sleep(1000);
            exit(0);
        }
        ok = kill(sleeper, SIGTERM) < 0 && kill(mine, SIGTERM) == 0 && waitpid(mine, &status, 0) == mine;
        ok &= nice(-1) == -1 && nice(0) == 0 && nice(1) == 1;
        perm_put(ok, 'k');

        int32_t r = run_capture("/bin/id", out, sizeof(out), &got);
        perm_put(r == 0 && same_text(out, got, "uid=100(user) gid=100(users) euid=0(root)\n"), 'e');
        ok = mknod("/tmp/mydev", 0020000 | 0666, 0x401) < 0 && stat("/tmp/mydev", &st) < 0;
        ok &= mknod("/tmp/myfifo", S_IFIFO | 0644, 0) == 0 && stat("/tmp/myfifo", &st) == 0 && st.mode == (S_IFIFO | 0644);
        ok &= st.uid == 100 && unlink("/tmp/myfifo") == 0;
        perm_put(ok, 'n');
        exit(0);
    }
    waitpid(c, &status, 0);
    /* 回到 root：root 能删用户的文件；root 也执行不了一个执行位都没有的程序，有了就能；0700 的目录 root 照样进 */
    ok = perm_n == 9 && unlink("/tmp/mine") == 0;
    ok &= mknod("/tmp/rdev", 0020000 | 0666, 0x401) == 0 && stat("/tmp/rdev", &st) == 0 && (st.mode & 0170000) == 0020000;
    ok &= run_capture("/tmp/hx", out, sizeof(out), &got) == 42 && chmod("/tmp/hx", 0755) == 0;
    ok &= run_capture("/tmp/hx", out, sizeof(out), &got) == 0 && got > 0;
    ok &= stat("/tmp/priv/f", &st) == 0;
    ok &= chmod("/tmp/hx", 04755) == 0 && chown("/tmp/hx", 100, 100) == 0 && stat("/tmp/hx", &st) == 0;
    ok &= st.uid == 100 && st.gid == 100 && st.mode == 0100755;  /* setuid 位清掉了 */
    kill(sleeper, SIGKILL);
    waitpid(sleeper, &status, 0);
    write(1, (const char *)perm_res, (uint32_t)perm_n);
    fd = open("/tmp/rdev", O_WRONLY);           /* 4,1 就是第 6 行：! 从这个新建的设备文件写出去 */
    write(fd, ok ? "!" : "?", 1);
    exit(0);
#elif SELFTEST == SELFTEST_ALL_EXIT
    write(1, "bye", 3);
    exit(2);
#else
#error "未知的用户态 SELFTEST"
#endif

    /*
     * 复制出一个子进程（任务 3）。子进程换成硬盘上的 /bin/letter，由它每 2 个 tick 写一个 C；
     * 父进程接着往下走，写 B。fork 之后两者的栈一开始是同一批物理页，谁先写谁复制一份。
     * 万一 exec 失败（比如没有硬盘），子进程就用这里的代码按任务号写 C，不往下走：下面是 init 自己的事。
     */
    char letter;
    if (fork() == 0) {
        char *argv[] = { "letter", "C", 0 };
        execv("/bin/letter", argv);
        letter = (char)('A' + getpid() - 1);
        for (;;) {
            write(1, &letter, 1);
            sleep(2);
        }
    }
    letter = (char)('A' + getpid() - 1);

#if !defined(SELFTEST)
    /*
     * 再拉起 shell（任务 4），做法与 Linux 0.11 的 init() 相同：子进程关掉继承来的 1 号描述符（第 6 行），
     * 把控制台 /dev/tty0 打开三次，正好得到 0、1、2 号描述符，再 exec /bin/sh。有串口的话，再在串口上拉起 login（任务 5）。
     * 之后 init 一边写 B，一边收走结束了的子进程；shell 或 login 退出了就再拉起一个。一下子就退出了
     * （比如没有硬盘，exec 不了），就隔 1 秒再试，免得一刻不停地 fork；fork 失败了也隔 1 秒再试。
     * init 还每隔大约 5 秒调用一次 sync，把缓冲区缓存里改过的块写回硬盘，就像 Linux 0.11 的 update 进程。
     * 自检内核不拉起 shell：它们要用控制台窗口检查键盘回显，shell 的提示符会混进去。
     */
    int32_t (*start[2])(void) = { start_shell, start_login };
    int32_t pids[2] = { -1, -1 };
    int age[2] = { 0, 0 }, wait[2] = { 0, 0 };  /* 活了几圈（每圈约 2 个 tick）；还要等几圈再拉起 */
    int32_t serial = open("/dev/ttyS0", O_RDWR);    /* 打不开就是没有串口（QEMU 用 -serial none 启动） */
    int n = serial >= 0 ? 2 : 1;
    close(serial);
    int rounds = 0;
    for (;;) {
        for (int k = 0; k < n; k++) {
            if (pids[k] < 0 && wait[k]-- <= 0) {
                pids[k] = start[k]();
                age[k] = 0;
                if (pids[k] < 0)
                    wait[k] = 50;
            }
        }
        write(1, &letter, 1);
        sleep(2);
        age[0]++, age[1]++;
        if (++rounds == 250) {      /* 250 圈约 5 秒 */
            rounds = 0;
            sync();
        }
        int32_t pid;
        while ((pid = waitpid(-1, 0, WNOHANG)) > 0)
            for (int k = 0; k < n; k++)
                if (pid == pids[k]) {
                    pids[k] = -1;
                    wait[k] = age[k] < 50 ? 50 : 0;
                }
    }
#else
    for (;;) {
        write(1, &letter, 1);
        sleep(2);
    }
#endif
}
