/*
 * mv 文件 新名字、mv 文件 目录：改名，或者搬进一个目录。没有 rename 系统调用，就像早年的 Unix 一样，
 * 先 link 一个新名字，再 unlink 旧的：文件的内容一个字节也不用搬，只是目录里换了一项。
 * 新名字已经是一个文件，先把它删掉；新名字就是这个文件本身（同一个 i 节点），什么也不做。
 * 目录搬不了：还得改它里面的 ..，那要 rename 才行。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    struct stat from, to;
    char path[128];

    if (argc != 3) {
        eprint("usage: mv file newname\n");
        return 2;
    }
    if (stat(argv[1], &from) < 0) {
        eprint("mv: ");
        eprint(argv[1]);
        eprint(": no such file\n");
        return 1;
    }
    if ((from.mode & 0170000) == 0040000) {
        eprint("mv: ");
        eprint(argv[1]);
        eprint(": cannot move a directory\n");
        return 1;
    }
    const char *dst = argv[2];
    if (stat(dst, &to) == 0 && (to.mode & 0170000) == 0040000) {
        /* 搬进目录：目录名加上 / 和原来的最后一段名字 */
        const char *base = argv[1];
        for (const char *p = argv[1]; *p; p++)
            if (*p == '/' && p[1])
                base = p + 1;
        uint32_t n = 0;
        for (const char *p = dst; *p && n < sizeof(path) - 1; p++)
            path[n++] = *p;
        if (n && path[n - 1] != '/' && n < sizeof(path) - 1)
            path[n++] = '/';
        for (const char *p = base; *p && *p != '/' && n < sizeof(path) - 1; p++)
            path[n++] = *p;
        path[n] = '\0';
        dst = path;
    }
    if (stat(dst, &to) == 0) {
        if (to.ino == from.ino)
            return 0;               /* 搬到它自己身上 */
        if ((to.mode & 0170000) == 0040000 || unlink(dst) < 0) {
            eprint("mv: ");
            eprint(dst);
            eprint(": cannot replace\n");
            return 1;
        }
    }
    if (link(argv[1], dst) < 0 || unlink(argv[1]) < 0) {
        eprint("mv: ");
        eprint(dst);
        eprint(": cannot move\n");
        return 1;
    }
    return 0;
}
