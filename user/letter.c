/* letter X：每 2 个 tick 写一个字母 X，永不结束。fork 出的任务 3 exec 它，第 6 行的 C 就是它写的。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char letter = argc > 1 ? argv[1][0] : '?';
    for (;;) {
        write(1, &letter, 1);
        sleep(2);
    }
}
