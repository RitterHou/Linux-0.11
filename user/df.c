/* df：文件系统一共多少数据块、多少 i 节点，各还剩多少（每块 1 KB）；缓冲区缓存里有几块改过还没写回硬盘。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    struct fsinfo info;

    if (fsinfo(&info) < 0) {
        print("df: no file system\n");
        return 1;
    }
    print("blocks: ");
    print_num(info.blocks - info.free_blocks);
    print(" used, ");
    print_num(info.free_blocks);
    print(" free; inodes: ");
    print_num(info.inodes - info.free_inodes);
    print(" used, ");
    print_num(info.free_inodes);
    print(" free\n");
    print("cache: ");                       /* 缓冲区缓存：改过还没写回的块，sync 时写回 */
    print_num(info.dirty);
    print(" of ");
    print_num(info.buffers);
    print(" buffers dirty; ");
    print_num(info.written);
    print(" blocks written since boot\n");
    return 0;
}
