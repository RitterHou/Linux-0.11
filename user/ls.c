/*
 * ls [-l] [目录]：列出目录（不给就是当前目录）里的每一项：类型、大小、i 节点号、名字。目录就是一个文件，内容是一条条
 * 16 字节的目录项（2 字节 i 节点号加 14 字节名字），直接 read 出来；大小和类型再用 stat 去问。
 * 早年的 Unix 的 ls 也是这样直接读目录文件。
 * -l 换一种长格式：类型和权限（rwx 各三位：主人、同组、别人）、链接数、主人、组、大小、修改时间（UTC）、名字，
 * 和 Unix 的 ls -l 差不多。
 */
#include "ulib.h"

static void print_mode(uint16_t mode)
{
    uint32_t type = mode & 0170000;
    char s[11] = "----------";
    s[0] = type == 0040000 ? 'd' : type == 0020000 ? 'c' : '-';
    for (int i = 0; i < 9; i++)
        if (mode & (0400 >> i))
            s[1 + i] = "rwx"[i % 3];
    print(s);
}

int main(int argc, char **argv)
{
    int lng = argc > 1 && strcmp(argv[1], "-l") == 0;
    const char *dir = argc > 1 + lng ? argv[1 + lng] : ".";
    struct { uint16_t inode; char name[14]; } de;
    char path[128], name[15];       /* 内核的路径最长 127 个字节 */
    struct stat st;

    int32_t fd = open(dir, 0);
    if (fd < 0 || stat(dir, &st) < 0 || (st.mode & 0170000) != 0040000) {
        print("ls: ");
        print(dir);
        print(": not a directory\n");
        return 1;
    }
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inode == 0)
            continue;
        for (int i = 0; i < 14; i++)
            name[i] = de.name[i];
        name[14] = '\0';
        uint32_t len = 0;
        for (const char *p = dir; *p && len < sizeof(path) - 16; p++)   /* 留出 /、14 字节的名字和结尾的 0 */
            path[len++] = *p;
        if (len == 0 || path[len - 1] != '/')
            path[len++] = '/';
        for (const char *p = name; *p; p++)
            path[len++] = *p;
        path[len] = '\0';
        if (stat(path, &st) < 0)
            continue;
        if (lng) {
            print_mode(st.mode);
            print_width(st.nlinks, 3, ' ');
            print_width(st.uid, 5, ' ');
            print_width(st.gid, 5, ' ');
            print_width(st.size, 8, ' ');
            print(" ");
            print_time(st.mtime, 0);
            print(" ");
        } else {
            uint32_t type = st.mode & 0170000;
            print(type == 0040000 ? "d" : type == 0020000 ? "c" : "-");
            print_width(st.size, 8, ' ');
            print_width(st.ino, 4, ' ');
            print("  ");
        }
        print(name);
        print("\n");
    }
    close(fd);
    return 0;
}
