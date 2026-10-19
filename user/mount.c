/*
 * mount：不带参数，列出挂着的文件系统，例如 /dev/hdb on /mnt；mount 设备 目录：把设备（/dev/hdb）上的文件系统
 * 挂到目录上，只有 root 能挂。见 fs.c 的 fs_do_mount()。
 */
#include "ulib.h"

/* 设备号换成名字：硬盘的主设备号是 3，次设备号 0 是 hda、5 是 hdb */
static const char *dev_name(uint32_t dev)
{
    return dev == 0x300 ? "/dev/hda" : dev == 0x305 ? "/dev/hdb" : "?";
}

int main(int argc, char **argv)
{
    struct fsinfo info;

    if (argc == 1) {
        for (uint32_t n = 0; fsinfo_at(n, &info) == 0; n++) {
            print(dev_name(info.dev));
            print(" on ");
            print(info.on);
            print("\n");
        }
        return 0;
    }
    if (argc != 3) {
        eprint("usage: mount [device dir]\n");
        return 2;
    }
    if (mount(argv[1], argv[2]) < 0) {
        eprint("mount: ");
        eprint(argv[1]);
        eprint(": cannot mount\n");
        return 1;
    }
    return 0;
}
