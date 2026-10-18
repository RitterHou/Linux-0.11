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
