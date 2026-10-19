/*
 * passwd [用户]：改密码。不给用户就是自己。普通用户只能改自己的，要先输对原来的密码；root 能改任何人的，不用输。
 * 新密码要输两遍。密码的散列存在 /etc/shadow 里，只有 root 能写，所以 passwd 是 root 的 setuid 程序：
 * 谁运行它都能改 shadow，但它自己按实际用户号检查你能改谁的。这正是 setuid 的用处。
 */
#include "ulib.h"

static char text[2048];

int main(int argc, char **argv)
{
    struct passwd me;
    char old[64], new1[64], new2[64], hash[17];
    uint32_t uid = (uint32_t)getuid();

    if (!getpwuid(uid, &me)) {
        eprint("passwd: who are you?\n");
        return 1;
    }
    const char *name = argc > 1 ? argv[1] : me.name;
    struct passwd pw;
    if (!getpwnam(name, &pw)) {
        eprint("passwd: ");
        eprint(name);
        eprint(": no such user\n");
        return 1;
    }
    if (uid != 0 && pw.uid != uid) {
        eprint("passwd: you may only change your own password\n");
        return 1;
    }
    if (uid != 0 && (read_password("old password: ", old, sizeof(old)) < 0 || !check_password(name, old))) {
        eprint("passwd: wrong password\n");
        return 1;
    }
    if (read_password("new password: ", new1, sizeof(new1)) <= 0 ||
        read_password("retype new password: ", new2, sizeof(new2)) < 0 || strcmp(new1, new2) != 0) {
        eprint("passwd: passwords do not match\n");
        return 1;
    }
    hash_password(name, new1, hash);

    /* 读进整个 shadow，换掉这个用户那一行的散列，再整个写回去 */
    int32_t fd = open("/etc/shadow", O_RDONLY), n = 0, got;
    if (fd < 0) {
        eprint("passwd: cannot read /etc/shadow\n");
        return 1;
    }
    while (n < (int32_t)sizeof(text) - 1 && (got = read(fd, text + n, sizeof(text) - 1 - (uint32_t)n)) > 0)
        n += got;
    close(fd);
    text[n] = '\0';
    fd = open("/etc/shadow", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        eprint("passwd: cannot write /etc/shadow\n");
        return 1;
    }
    uint32_t len = strlen(name);
    for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        int match = (uint32_t)(end - line) > len && line[len] == ':';
        for (uint32_t i = 0; match && i < len; i++)
            match = line[i] == name[i];
        if (match) {
            write(fd, name, len);
            write(fd, ":", 1);
            write(fd, hash, 16);
        } else {
            write(fd, line, (uint32_t)(end - line));
        }
        write(fd, "\n", 1);
        line = *end ? end + 1 : end;
    }
    close(fd);
    print("password changed\n");
    return 0;
}
