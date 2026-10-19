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
     *   c：alarm(1) 之后一直 sleep(5)：大约 20 次之后 SIGALRM 来了，处理函数接住，正在睡的 sleep 被打断。
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
    int k = 0;
    while (!rang && k < 100) {
        sleep(5);
        k++;
    }
    int ok = rang == SIGALRM && k >= 15 && k <= 25;
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
     * 每一项通过就写一个字母，期望第 6 行（去掉任务 A 的 A）是 lwuoe：
     *   l：link 之后两个名字的 i 节点号一样，链接数是 2，从新名字读到的就是原来的内容。
     *   w：从一个名字写进去，从另一个名字马上读得到：内存里是同一个 i 节点。
     *   u：删掉原来的名字，另一个名字照常能读，链接数变回 1，块和 i 节点都没收回；再删掉它，才都收回。
     *   o：两个名字都删了，可文件还开着：照常能读；关掉以后才收回。
     *   e：目录不能 link；新名字已经存在（包括 . 和 ..）、源文件不存在、新名字所在的目录不存在，都返回 -1。
     */
    struct fsinfo before, now;
    struct stat a, b;
    char buf[16];
    fsinfo(&before);
    int32_t fd = open("/tmp/a", O_WRONLY | O_CREAT);
    write(fd, "hello", 5);
    close(fd);
    int ok = link("/tmp/a", "/tmp/b") == 0 && stat("/tmp/a", &a) == 0 && stat("/tmp/b", &b) == 0;
    ok &= a.ino == b.ino && a.nlinks == 2 && b.nlinks == 2;
    fd = open("/tmp/b", O_RDONLY);
    ok &= read(fd, buf, sizeof(buf)) == 5 && buf[0] == 'h' && buf[4] == 'o';
    close(fd);
    write(1, ok ? "l" : "L", 1);

    int32_t wfd = open("/tmp/a", O_WRONLY | O_APPEND), rfd = open("/tmp/b", O_RDONLY);
    write(wfd, "!", 1);
    ok = read(rfd, buf, sizeof(buf)) == 6 && buf[5] == '!';
    close(wfd);
    close(rfd);
    write(1, ok ? "w" : "W", 1);

    ok = unlink("/tmp/a") == 0 && stat("/tmp/a", &a) < 0 && stat("/tmp/b", &b) == 0 && b.nlinks == 1;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes - 1 && now.free_blocks == before.free_blocks - 1;
    ok &= unlink("/tmp/b") == 0;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes && now.free_blocks == before.free_blocks;
    write(1, ok ? "u" : "U", 1);

    fd = open("/tmp/c", O_RDWR | O_CREAT);
    write(fd, "xyz", 3);
    ok = link("/tmp/c", "/tmp/d") == 0 && unlink("/tmp/c") == 0 && unlink("/tmp/d") == 0;
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes - 1;    /* 还开着：没收回 */
    int32_t again = open("/tmp/c", O_RDONLY);
    ok &= again < 0;
    close(fd);
    fsinfo(&now);
    ok &= now.free_inodes == before.free_inodes && now.free_blocks == before.free_blocks;
    write(1, ok ? "o" : "O", 1);

    fd = open("/tmp/e", O_WRONLY | O_CREAT);
    close(fd);
    ok = link("/tmp", "/tmp/dir2") < 0 && link("/tmp/e", "/tmp/e") < 0 && link("/tmp/e", "/tmp/.") < 0;
    ok &= link("/nothing", "/tmp/f") < 0 && link("/tmp/e", "/nothing/f") < 0 && link("/tmp/e", "/tmp/abcdefghijklmno") < 0;
    ok &= stat("/tmp/e", &a) == 0 && a.nlinks == 1 && unlink("/tmp/e") == 0;
    write(1, ok ? "e" : "E", 1);
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
     * 把控制台 /dev/tty0 打开三次，正好得到 0、1、2 号描述符，再 exec /bin/sh。
     * 之后 init 一边写 B，一边收走结束了的子进程；shell 退出了就再拉起一个。shell 一下子就退出了
     * （比如没有硬盘，exec 不了），就隔 1 秒再试，免得一刻不停地 fork；fork 失败了也隔 1 秒再试。
     * init 还每隔大约 5 秒调用一次 sync，把缓冲区缓存里改过的块写回硬盘，就像 Linux 0.11 的 update 进程。
     * 自检内核不拉起 shell：它们要用控制台窗口检查键盘回显，shell 的提示符会混进去。
     */
    int32_t shell = start_shell();
    int age = 0, wait = 0;          /* shell 活了几圈（每圈约 2 个 tick）；还要等几圈再拉起 */
    int rounds = 0;
    for (;;) {
        write(1, &letter, 1);
        sleep(2);
        age++;
        if (++rounds == 250) {      /* 250 圈约 5 秒 */
            rounds = 0;
            sync();
        }
        int32_t pid;
        while ((pid = waitpid(-1, 0, WNOHANG)) > 0)
            if (pid == shell) {
                shell = -1;
                wait = age < 50 ? 50 : 0;
            }
        if (shell < 0 && wait-- <= 0) {
            shell = start_shell();
            age = 0;
            if (shell < 0)
                wait = 50;
        }
    }
#else
    for (;;) {
        write(1, &letter, 1);
        sleep(2);
    }
#endif
}
