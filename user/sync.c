/* sync：把缓冲区缓存里改过的块都写回硬盘。关掉 QEMU 之前运行它，刚写的文件就不会丢。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (sync() < 0) {
        eprint("sync: write error\n");
        return 1;
    }
    return 0;
}
