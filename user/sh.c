/*
 * sh：一个很小的 shell。读一行，按空格切成参数，fork 一个子进程去 exec 程序，等它结束，再读下一行。
 *   - 命令名里没有 / 的，到 /bin 下找：ls 就是 /bin/ls；
 *   - 最后一个参数是 & 的，在后台运行，不等它；每次读命令之前收走已经结束的后台程序；
 *   - 内建命令：exit 退出 shell（init 会再拉起一个），help 显示能用的命令。
 * Linux 0.11 的 init 运行的是磁盘上的 /bin/sh（当年的 bash），道理相同：fork、exec、wait。
 */
#include "ulib.h"

#define LINE_LEN 128
#define MAX_ARGS 16

/* 原地把 line 按空格切开，argv 指向每一段，返回段数。 */
static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < MAX_ARGS) {
        while (*p == ' ')
            *p++ = '\0';
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    argv[argc] = 0;
    return argc;
}

static void report(const char *name, int32_t st)
{
    if (st & 0x100) {               /* 被异常结束：0x100 加异常号 */
        print("sh: ");
        print(name);
        print(" killed by exception ");
        print_num((uint32_t)st & 0xFF);
        print("\n");
    }
}

int main(int sh_argc, char **sh_argv)
{
    (void)sh_argc, (void)sh_argv;
    char line[LINE_LEN], path[64];
    char *argv[MAX_ARGS + 1];
    int32_t st;

    print("shell on hda: try help, ls /bin, cat /hello.txt, ps\n");
    for (;;) {
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;                       /* 收走结束了的后台程序 */
        print("$ ");
        int32_t n = read(0, line, LINE_LEN - 1);
        if (n <= 0)
            continue;
        line[n] = '\0';
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        int argc = split(line, argv);
        if (argc == 0)
            continue;
        int background = strcmp(argv[argc - 1], "&") == 0;
        if (background) {
            argv[--argc] = 0;
            if (argc == 0)
                continue;
        }
        if (strcmp(argv[0], "exit") == 0)
            return 0;
        if (strcmp(argv[0], "help") == 0) {
            print("commands: ls [dir], cat file..., echo text, ps, hello, check, letter X &, exit\n");
            continue;
        }

        uint32_t len = 0;
        int has_slash = 0;
        for (const char *p = argv[0]; *p; p++)
            has_slash |= *p == '/';
        if (!has_slash)
            for (const char *p = "/bin/"; *p; p++)
                path[len++] = *p;
        for (const char *p = argv[0]; *p && len < sizeof(path) - 1; p++)
            path[len++] = *p;
        path[len] = '\0';

        int32_t pid = fork();
        if (pid == 0) {
            execv(path, argv);
            print("sh: ");
            print(argv[0]);
            print(": not found\n");
            exit(127);
        }
        if (pid < 0) {
            print("sh: fork failed\n");
            continue;
        }
        if (background) {
            print("[");
            print_num((uint32_t)pid);
            print("]\n");
            continue;
        }
        waitpid(pid, &st, 0);
        report(argv[0], st);
    }
}
