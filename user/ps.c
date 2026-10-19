/*
 * ps [-l]：列出所有任务：任务号、父进程、状态、名字。状态：R 能运行，S 在 sleep 里，Z 僵尸，W 在 waitpid 里，
 * B 在等待队列上（等键盘、等硬盘）。-l 再加上每轮的 tick 数（PRI）、nice 值（NI）和用了多少 CPU 时间（TIME，秒）。
 */
#include "ulib.h"

/* 有符号的数右对齐到 width 格 */
static void print_signed(int32_t n, uint32_t width)
{
    uint32_t a = n < 0 ? 0u - (uint32_t)n : (uint32_t)n, len = n < 0 ? 2 : 1;
    for (uint32_t m = a; m >= 10; m /= 10)
        len++;
    for (; len < width; len++)
        print(" ");
    print_int(n);
}

int main(int argc, char **argv)
{
    static const char states[] = "?RSZWB";
    struct taskinfo info;
    int lng = argc > 1 && strcmp(argv[1], "-l") == 0;

    print(lng ? "PID PPID S PRI  NI    TIME NAME\n" : "PID PPID S NAME\n");
    for (int pid = 0; pid < 16; pid++) {
        if (taskinfo(pid, &info) < 0)
            continue;
        if (pid < 10)
            print(" ");
        print_num((uint32_t)pid);
        print("   ");
        if (info.parent < 10)
            print(" ");
        print_num((uint32_t)info.parent);
        char s[4] = { ' ', info.state >= 1 && info.state <= 5 ? states[info.state] : '?', ' ', '\0' };
        print(s);
        if (lng) {
            print_width((uint32_t)info.priority, 3, ' ');
            print_signed(info.nice, 4);
            print_width(info.ticks / 100, 5, ' ');     /* 秒，两位小数：一秒 100 个 tick */
            print(".");
            print_width(info.ticks % 100, 2, '0');
            print(" ");
        }
        print(info.name);
        print("\n");
    }
    return 0;
}
