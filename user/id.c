/*
 * id：打印自己的用户号和组号，后面括号里是 /etc/passwd、/etc/group 里的名字，例如 uid=100(user) gid=100(users)。
 * 有效的和实际的不一样（在运行 setuid 程序）时，再打印 euid=、egid=。
 */
#include "ulib.h"

static void show(const char *label, uint32_t id, int is_group)
{
    struct passwd pw;
    char group[16];
    print(label);
    print_num(id);
    if (is_group ? getgrgid(id, group, sizeof(group)) : getpwuid(id, &pw)) {
        print("(");
        print(is_group ? group : pw.name);
        print(")");
    }
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    uint32_t uid = (uint32_t)getuid(), euid = (uint32_t)geteuid(), gid = (uint32_t)getgid(), egid = (uint32_t)getegid();
    show("uid=", uid, 0);
    show(" gid=", gid, 1);
    if (euid != uid)
        show(" euid=", euid, 0);
    if (egid != gid)
        show(" egid=", egid, 1);
    print("\n");
    return 0;
}
