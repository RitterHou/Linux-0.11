/*
 * mkfifo 文件…：新建命名管道。它在目录里有名字，内容不在硬盘上：一个进程打开它来读，另一个打开它来写，
 * 两边就像 | 接起来的一样。读的一头打开时等到有人打开写的一头，反过来也一样。见 file.c 的 fifo_open()。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        eprint("usage: mkfifo file...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (mknod(argv[i], S_IFIFO | 0644, 0) < 0) {
            eprint("mkfifo: ");
            eprint(argv[i]);
            eprint(": cannot make\n");
            status = 1;
        }
    }
    return status;
}
