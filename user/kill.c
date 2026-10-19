/*
 * kill [-信号] 任务号…：给任务发信号，默认是 SIGTERM（15）。kill -9 发 SIGKILL，忽略了也躲不掉。
 * 任务号是负数就发给那个进程组，例如 kill -2 -4 把 SIGINT 发给 4 号任务那一组。
 */
#include "ulib.h"

/* 把十进制数（可以带负号）转成整数；不是数、太大返回 0。4294967297 不会回绕成 1，把信号发给别的任务。 */
static int parse(const char *s, int32_t *out)
{
    int neg = *s == '-';
    uint32_t n;

    if (!parse_uint(s + neg, 10, 0x7FFFFFFF, &n))
        return 0;
    *out = neg ? -(int32_t)n : (int32_t)n;
    return 1;
}

int main(int argc, char **argv)
{
    int32_t sig = SIGTERM;
    int i = 1, status = 0;

    if (argc > 2 && argv[1][0] == '-') {     /* 第一个参数是 -信号：后面至少还要有一个任务号 */
        if (!parse(argv[1] + 1, &sig) || sig < 1 || sig > 32) {
            eprint("kill: ");
            eprint(argv[1]);
            eprint(": bad signal\n");
            return 2;
        }
        i = 2;
    }
    if (i >= argc) {
        eprint("usage: kill [-signal] pid...\n");
        return 2;
    }
    for (; i < argc; i++) {
        int32_t pid;
        if (!parse(argv[i], &pid) || kill(pid, sig) < 0) {
            eprint("kill: ");
            eprint(argv[i]);
            eprint(": cannot signal\n");
            status = 1;
        }
    }
    return status;
}
