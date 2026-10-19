/*
 * df：每个挂着的文件系统一行：挂在哪里，一共多少数据块、多少 i 节点，各用了多少、还剩多少（每块 1 KB）；
 * 最后一行是缓冲区缓存：几块改过还没写回硬盘，开机以来写回了多少块。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    struct fsinfo info;

    if (fsinfo(&info) < 0) {
        print("df: no file system\n");
        return 1;
    }
    for (uint32_t n = 0; fsinfo_at(n, &info) == 0; n++) {
        print(info.on);
        print(": blocks: ");
        print_num(info.blocks - info.free_blocks);
        print(" used, ");
        print_num(info.free_blocks);
        print(" free; inodes: ");
        print_num(info.inodes - info.free_inodes);
        print(" used, ");
        print_num(info.free_inodes);
        print(" free\n");
    }
    print("cache: ");                       /* 缓冲区缓存：改过还没写回的块，sync 时写回 */
    print_num(info.dirty);
    print(" of ");
    print_num(info.buffers);
    print(" buffers dirty; ");
    print_num(info.written);
    print(" blocks written since boot\n");
    return 0;
}
