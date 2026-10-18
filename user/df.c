/* df：文件系统一共多少数据块、多少 i 节点，各还剩多少。每块 1 KB。 */
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
    return 0;
}
