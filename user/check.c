/*
 * check：检查 exec 把自己装对了。全部通过就打印 check ok，以 7 退出；有一项不对就打印 check bad，以 1 退出。
 *   - 有初值的全局变量（.data）是文件里的值；没有初值的（.bss）全是 0；
 *   - argc 和 argv 是 exec 传来的；字符串常量（.rodata）能读。
 * check crash：往自己的代码里写，代码页是只读的，期望被缺页异常结束。
 * check badptr：把自己代码页的地址交给要写用户内存的系统调用，内核应当逐个返回 -1，自己照常运行，
 * 最后打印 badptr ok。内核要是等到写的时候才发现那一页是只读的，就只能停机了。
 */
#include "ulib.h"

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
