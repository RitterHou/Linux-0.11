/*
 * sleep 秒数：睡上这么多秒。用 alarm 定一个闹钟，再 pause 等信号：闹钟一响，SIGALRM 的处理函数记一笔，
 * pause 返回。早年 Unix 的 C 库里，sleep() 就是这样写的。中途按 Ctrl+C，SIGINT 照样结束它。
 * 闹钟要是恰好在检查 rang 之后、调用 pause 之前响，pause 就等不到它了：这一小段没法避免，
 * 要靠 sigsuspend 那种“放开信号和睡下一步完成”的系统调用，这里没有。
 */
#include "ulib.h"

static volatile int rang;

static void on_alarm(int sig)
{
    (void)sig;
    rang = 1;
}

int main(int argc, char **argv)
{
    uint32_t n;

    if (argc != 2 || !parse_uint(argv[1], 10, 0xFFFFFFFF, &n)) {    /* 4294967296 不会回绕成 0 */
        eprint("usage: sleep seconds\n");
        return 2;
    }
    if (n == 0)
        return 0;
    signal(SIGALRM, on_alarm);
    alarm(n);
    while (!rang)
        pause();
    return 0;
}
