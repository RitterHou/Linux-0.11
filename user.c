/*
 * 用户程序：任务 B 在特权级 3（用户态）运行这里的代码。
 *
 * 链接脚本把本文件的代码、只读数据和数据单独放进按 4 KB 对齐的 .user 段，
 * paging.c 只给这几页打开“用户”位，其中代码所在的页只读。用户栈在 0x40000000 起的 4 MB 里，
 * 用到哪一页，内核才在缺页异常里分配哪一页（见 paging.c 的 handle_page_fault()）。内核的代码、数据和显存对这里的代码都不可访问，
 * cli、hlt、in、out 等特权指令也不能执行，要做任何事都只能用 int 0x80 请内核帮忙。
 * 所以这里只包含 syscall.h，不包含内核的 kernel.h。
 */
#include "selftest.h"
#include "syscall.h"

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_WRITE_KERNEL
extern volatile uint32_t timer_ticks;   /* 内核的变量，所在的页没有用户位 */
#endif

#if !defined(SELFTEST)
static int32_t start_shell(void)
{
    int32_t pid = fork();
    if (pid == 0) {
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
     * 自检内核不拉起 shell：它们要用控制台窗口检查键盘回显，shell 的提示符会混进去。
     */
    int32_t shell = start_shell();
    int age = 0, wait = 0;          /* shell 活了几圈（每圈约 2 个 tick）；还要等几圈再拉起 */
    for (;;) {
        write(1, &letter, 1);
        sleep(2);
        age++;
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
