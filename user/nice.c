/*
 * nice [-n 增量] [命令 参数…]：把 nice 值加上增量（不给就是 10），再运行命令；命令名里没有 / 的到 /bin 下找，和 shell 一样。
 * nice 值在 -20～19 之间，越大越谦让：调度时每一轮补的 tick 越少（见 sched.c）。只写 nice 不带命令，打印现在的 nice 值。
 * 子进程继承 nice 值，exec 也不改它，所以 nice 自己调完再 exec，命令就在新的 nice 值下运行。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int32_t inc = 10;
    int i = 1;

    if (argc == 1) {
        print_int(nice(0));
        print("\n");
        return 0;
    }
    if (strcmp(argv[1], "-n") == 0) {
        uint32_t n;
        const char *s = argc > 2 ? argv[2] : "";
        int neg = *s == '-';
        if (!parse_uint(s + neg, 10, 40, &n)) {
            eprint("usage: nice [-n increment] [command args...]\n");
            return 2;
        }
        inc = neg ? -(int32_t)n : (int32_t)n;
        i = 3;
    }
    int32_t old = nice(0);
    nice(inc);
    if (inc < 0 && old > -20 && nice(0) == old)
        eprint("nice: cannot set niceness\n");    /* 只有 root 能调低；命令照样运行，和 coreutils 一样 */
    if (i >= argc) {                /* nice -n 3：没有命令，打印调过以后的值 */
        print_int(nice(0));
        print("\n");
        return 0;
    }
    char path[128];
    uint32_t len = 0;
    int has_slash = 0;
    for (const char *p = argv[i]; *p; p++)
        has_slash |= *p == '/';
    if (!has_slash)
        for (const char *p = "/bin/"; *p; p++)
            path[len++] = *p;
    if (len + strlen(argv[i]) > sizeof(path) - 1) {
        eprint("nice: ");
        eprint(argv[i]);
        eprint(": name too long\n");
        return 127;
    }
    for (const char *p = argv[i]; *p; p++)
        path[len++] = *p;
    path[len] = '\0';
    execv(path, argv + i);
    eprint("nice: ");
    eprint(argv[i]);
    eprint(": not found\n");
    return 127;
}
