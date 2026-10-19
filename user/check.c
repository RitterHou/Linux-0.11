/*
 * check：检查 exec 把自己装对了。全部通过就打印 check ok，以 7 退出；有一项不对就打印 check bad，以 1 退出。
 *   - 有初值的全局变量（.data）是文件里的值；没有初值的（.bss）全是 0；
 *   - argc 和 argv 是 exec 传来的；字符串常量（.rodata）能读。
 * check crash：往自己的代码里写，代码页是只读的，期望被缺页异常结束。
 * check badptr：把自己代码页的地址交给要写用户内存的系统调用，内核应当逐个返回 -1，自己照常运行，
 * 最后打印 badptr ok。内核要是等到写的时候才发现那一页是只读的，就只能停机了。
 * check fds：打印开着的描述符，例如 fds: 0 1 2。看 exec 之后还留着哪些描述符（close-on-exec 关掉了哪些）。
 * check spin：在用户态空转半秒（50 个 tick），给 time 和 nice 一个一直要 CPU 的程序。
 * check raw：把终端换成原始模式、关掉回显，读 4 个字节（一个一个地交来，不用敲回车），打印它们的值，再换回来。
 * check lazy [all|late]：程序里有一张 64 KB（16 页）的只读表，每页开头记着页号。不带参数只读第一页，all 读遍 16 页，
 * 读完都睡 3 秒（好让别人看第 3 行的空闲页数）；late 先睡 1 秒再读遍 16 页。每页的记号都对，以 0 退出，打印 lazy ok。
 * 睡之前还往 .data 里的 mine 写下自己的任务号，睡醒再看：可写的页每个进程一份，别的 check 写不到这里。
 * 程序的页是用到才从硬盘读进来的：只读第一页，就只多用了一页；两个 check 一起运行，只读的页共用。
 */
#include "ulib.h"

/* 16 页的只读表：放在 .rodata，和代码一起映射成只读，能和别的进程共用 */
#define PAGE_WORDS 1024
static const uint32_t table[16 * PAGE_WORDS] = {
    [0 * PAGE_WORDS] = 1, [1 * PAGE_WORDS] = 2, [2 * PAGE_WORDS] = 3, [3 * PAGE_WORDS] = 4,
    [4 * PAGE_WORDS] = 5, [5 * PAGE_WORDS] = 6, [6 * PAGE_WORDS] = 7, [7 * PAGE_WORDS] = 8,
    [8 * PAGE_WORDS] = 9, [9 * PAGE_WORDS] = 10, [10 * PAGE_WORDS] = 11, [11 * PAGE_WORDS] = 12,
    [12 * PAGE_WORDS] = 13, [13 * PAGE_WORDS] = 14, [14 * PAGE_WORDS] = 15, [15 * PAGE_WORDS] = 16,
};

/* 读表的前 n 页，看每页开头的记号对不对。volatile：不让 clang 把读表省掉 */
static int read_table(int n)
{
    int ok = 1;
    for (int k = 0; k < n; k++)
        ok &= ((const volatile uint32_t *)table)[k * PAGE_WORDS] == (uint32_t)(k + 1);
    return ok;
}

static volatile int32_t mine = -1;  /* .data */

/* volatile：否则 clang 看出它们从没被改过，直接把值算出来，.data 和 .bss 就根本不会生成 */
volatile int initialized = 12345;
volatile char zeros[5000];          /* 跨过一页多：.bss 不在文件里，exec 要分出清零的页 */

/*
 * check malloc：分配器自己的检查，结果只用退出码报告。
 *   - 先后 free 的两块挨着的空块，能并成一块再分出去（不管 free 的顺序）；
 *   - 别人直接 sbrk 了一段，malloc 跨过它，不会卡住，也不会把那一段分出去；
 *   - 堆末尾的块 realloc 变大，原地变，内容不变；不在末尾的，搬过去，内容也不变；
 *   - 32 个槽随机 malloc、free 两千次：每块都对齐到 8 字节，填上自己的花样，free 之前核对没被别人改过。
 */
static int check_malloc(void)
{
    int ok = 1;
    char *a = malloc(100), *b = malloc(100), *c = malloc(100);
    free(a);
    free(b);                                /* a 先空出来：free(b) 时 a 不在 b 后面，要靠 malloc 扫描时并上 */
    ok &= malloc(200) == a;
    free(a);
    free(c);

    char *p = malloc(16);
    char *gap = sbrk(4096);
    char *q = malloc(16), *r = malloc(16);
    ok &= p && q && r && gap != (void *)-1 && (q >= gap + 4096 || q + 16 <= gap) && (r >= gap + 4096 || r + 16 <= gap);
    for (int i = 0; i < 4096; i++)
        gap[i] = 0;                         /* 全是 0：malloc 要是把这里当成块头，大小是 0，会原地打转 */
    q[0] = 2;
    ok &= gap[0] == 0 && gap[4095] == 0;

    char *big = malloc(50000);
    for (int i = 0; i < 50000; i++)
        big[i] = (char)i;
    char *bigger = realloc(big, 100000);
    ok &= bigger == big;                    /* 在堆末尾：原地变大 */
    for (int i = 0; i < 50000; i++)
        ok &= bigger[i] == (char)i;
    char *moved = realloc(p, 1000);         /* p 后面有别的块：搬 */
    ok &= moved != p && moved[0] == p[0];

    static char *slot[32];
    static uint32_t len[32];
    uint32_t seed = 12345;
    for (int op = 0; op < 2000 && ok; op++) {
        seed = seed * 1103515245 + 12345;
        int k = (int)((seed >> 16) % 32);
        if (slot[k]) {
            for (uint32_t i = 0; i < len[k]; i++)
                ok &= slot[k][i] == (char)(k + i);
            free(slot[k]);
            slot[k] = 0;
        } else {
            len[k] = 1 + (seed >> 8) % 3000;
            slot[k] = malloc(len[k]);
            ok &= slot[k] != 0 && ((uint32_t)slot[k] & 7) == 0;
            for (uint32_t i = 0; ok && i < len[k]; i++)
                slot[k][i] = (char)(k + i);
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "badptr") == 0) {
        void *code = (void *)main;
        int32_t fd = open("/hello.txt", 0);
        int ok = read(fd, code, 1) == -1;
        int32_t child = fork();
        if (child == 0)
            exit(3);
        ok &= waitpid(child, (int32_t *)code, 0) == -1;
        ok &= waitpid(child, 0, 0) == child;
        ok &= stat("/", (struct stat *)code) == -1;
        ok &= taskinfo(0, (struct taskinfo *)code) == -1;
        ok &= pipe((int32_t *)code) == -1;
        print(ok ? "badptr ok\n" : "badptr bad\n");
        return ok ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "brk") == 0) {
        /* 堆从程序的末尾开始；malloc 来的 100 KB 都能写、能读回来。结果只用退出码报告 */
        extern char __end[];
        int ok = brk(0) == (uint32_t)__end;
        uint32_t *p = malloc(100 * 1024);
        ok &= p != 0;
        for (uint32_t i = 0; ok && i < 25 * 1024; i++)
            p[i] = i;
        for (uint32_t i = 0; ok && i < 25 * 1024; i++)
            ok &= p[i] == i;
        return ok ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "fds") == 0) {
        print("fds:");
        for (int32_t fd = 0; fd < 8; fd++) {
            if (fcntl(fd, F_GETFD, 0) >= 0) {
                print(" ");
                print_num((uint32_t)fd);
            }
        }
        print("\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "spin") == 0) {
        uint32_t until = times(0) + 50;
        while (times(0) < until)
            for (volatile int i = 0; i < 100000; i++)   /* 隔一阵才问一次：系统调用在 QEMU 里很慢，问得勤了时间都记在内核态 */
                ;
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "raw") == 0) {
        struct termios t, saved;
        if (ioctl(0, TCGETS, &saved) < 0)
            return 1;
        t = saved;
        t.c_lflag &= ~(uint32_t)(ICANON | ECHO);
        ioctl(0, TCSETS, &t);
        unsigned char got[4];
        int n = 0;
        while (n < 4) {
            int32_t r = read(0, got + n, (uint32_t)(4 - n));
            if (r <= 0)
                break;
            n += r;
        }
        ioctl(0, TCSETS, &saved);
        for (int i = 0; i < n; i++) {
            print_num(got[i]);
            print(i + 1 < n ? " " : "\n");
        }
        return n == 4 ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "lazy") == 0) {
        int late = argc == 3 && strcmp(argv[2], "late") == 0, all = argc == 3 && strcmp(argv[2], "all") == 0;
        mine = getpid();
        if (late)
            sleep(100);
        int ok = read_table(all || late ? 16 : 1);
        if (!late)
            sleep(300);
        ok &= mine == getpid();
        print(ok ? "lazy ok\n" : "lazy bad\n");
        return ok ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "malloc") == 0)
        return check_malloc() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "crash") == 0) {
        *(volatile char *)main = 0;
        print("x\n");
        return 1;
    }
    int ok = initialized == 12345 && argc == 1 && strcmp(argv[0], "check") == 0;
    for (uint32_t i = 0; i < sizeof(zeros); i++)
        ok &= zeros[i] == 0;
    print(ok ? "check ok\n" : "check bad\n");
    return ok ? 7 : 1;
}
