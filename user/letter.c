/*
 * letter X：每 2 个 tick 往第 6 行写一个字母 X，永不结束。开机时 fork 出的任务 3 exec 它，第 6 行的 C 就是它写的；
 * 在 shell 里运行 letter D &，第 6 行就会多出 D。它自己打开 /dev/tty1，不管 1 号描述符指向哪里。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char letter = argc > 1 ? argv[1][0] : '?';
    int32_t fd = open("/dev/tty1", 0);
    if (fd < 0)
        fd = 1;
    for (;;) {
        write(fd, &letter, 1);
        sleep(2);
    }
}
