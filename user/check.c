/*
 * check：检查 exec 把自己装对了。全部通过就打印 check ok，以 7 退出；有一项不对就打印 check bad，以 1 退出。
 *   - 有初值的全局变量（.data）是文件里的值；没有初值的（.bss）全是 0；
 *   - argc 和 argv 是 exec 传来的；字符串常量（.rodata）能读。
 * check crash：往自己的代码里写，代码页是只读的，期望被缺页异常结束。
 */
#include "ulib.h"

/* volatile：否则 clang 看出它们从没被改过，直接把值算出来，.data 和 .bss 就根本不会生成 */
volatile int initialized = 12345;
volatile char zeros[5000];          /* 跨过一页多：.bss 不在文件里，exec 要分出清零的页 */

int main(int argc, char **argv)
{
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
