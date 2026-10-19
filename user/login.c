/*
 * login：问用户名和密码，对了就变成那个用户，进他的家目录，运行他的 shell。错了再问，三次都错就退出；
 * 在用户名那里按 Ctrl+D 也退出。要以 root 运行（init 拉起它，或者在 root 的 shell 里运行）：换用户要 root 的权限，
 * 读 /etc/shadow 也是。和 su 不同，它不是 setuid 程序，别人运行它，读不了 shadow，密码总是不对。
 * Linux 0.11 的 init 直接拉起 shell，没有 login；后来的 Unix 由 init 在每个终端上运行 getty，getty 再 exec login。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    char name[32], password[64];
    struct passwd pw;

    for (int tries = 0; tries < 3; tries++) {
        print("login: ");
        if (read_text_line(name, sizeof(name)) < 0) {
            print("\n");
            return 1;
        }
        if (read_password("password: ", password, sizeof(password)) >= 0 && getpwnam(name, &pw) &&
            check_password(name, password)) {
            if (setgid(pw.gid) < 0 || setuid(pw.uid) < 0) {
                eprint("login: cannot change user\n");
                return 1;
            }
            if (chdir(pw.home) < 0)
                chdir("/");
            char *sh_argv[] = { "sh", 0 };
            execv(pw.shell, sh_argv);
            eprint("login: cannot run the shell\n");
            return 1;
        }
        print("login incorrect\n");     /* 不说是用户名不对还是密码不对：免得别人试出有哪些用户 */
    }
    return 1;
}
