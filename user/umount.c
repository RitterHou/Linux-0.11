/* umount 设备或目录：卸下一块盘（/dev/hdb 或者它挂着的 /mnt）。有文件还开着就卸不了。见 fs.c 的 fs_do_umount()。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        eprint("usage: umount device|dir\n");
        return 2;
    }
    if (umount(argv[1]) < 0) {
        eprint("umount: ");
        eprint(argv[1]);
        eprint(": cannot unmount\n");
        return 1;
    }
    return 0;
}
