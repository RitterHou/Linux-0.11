/*
 * su [用户]：变成另一个用户（默认 root），运行他的 shell；退出那个 shell 就回到原来的。当前目录不变。
 * su 是 root 的 setuid 程序（mkfs 打包时设成 4755）：谁运行它，有效用户号都是 0，所以读得了只有 root 能读的
 * /etc/shadow，核对密码；对了，先 setgid 再 setuid 换成那个用户（先换用户就没有权限换组了），再 exec 他的 shell。
 * 实际用户号是 root 的（root 自己运行 su）不用输密码。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "root";
    struct passwd pw;
    char password[64];

    if (!getpwnam(name, &pw)) {
        eprint("su: ");
        eprint(name);
        eprint(": no such user\n");
        return 1;
    }
    if (getuid() != 0 && (read_password("password: ", password, sizeof(password)) < 0 || !check_password(name, password))) {
        eprint("su: wrong password\n");
        return 1;
    }
    if (setgid(pw.gid) < 0 || setuid(pw.uid) < 0) {
        eprint("su: cannot change user\n");
        return 1;
    }
    char *sh_argv[] = { "sh", 0 };
    execv(pw.shell, sh_argv);
    eprint("su: ");
    eprint(pw.shell);
    eprint(": cannot run\n");
    return 1;
}
