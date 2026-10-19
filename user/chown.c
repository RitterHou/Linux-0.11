/*
 * chown 主人[:组] 文件…：改文件的主人（和组）。主人、组可以写名字（查 /etc/passwd、/etc/group），也可以写号码。
 * 只有 root 能改，别人运行会报错：见 fs.c 的 fs_chown()。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        eprint("usage: chown owner[:group] file...\n");
        return 2;
    }
    char owner[32];
    const char *group = 0;
    uint32_t i = 0;
    for (; argv[1][i] && argv[1][i] != ':' && i < sizeof(owner) - 1; i++)
        owner[i] = argv[1][i];
    owner[i] = '\0';
    if (argv[1][i] == ':')
        group = argv[1] + i + 1;
    struct passwd pw;
    uint32_t uid, gid = 0xFFFFFFFF;                     /* 不给组就不改组 */
    if (!parse_uint(owner, 10, 0xFFFF, &uid)) {
        if (!getpwnam(owner, &pw)) {
            eprint("chown: ");
            eprint(owner);
            eprint(": no such user\n");
            return 1;
        }
        uid = pw.uid;
    }
    if (group && !parse_uint(group, 10, 0xFF, &gid) && !getgrnam(group, &gid)) {
        eprint("chown: ");
        eprint(group);
        eprint(": no such group\n");
        return 1;
    }
    int status = 0;
    for (int k = 2; k < argc; k++) {
        if (chown(argv[k], uid, gid) < 0) {
            eprint("chown: ");
            eprint(argv[k]);
            eprint(": cannot change owner\n");
            status = 1;
        }
    }
    return status;
}
