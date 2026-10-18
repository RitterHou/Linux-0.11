/*
 * sh：一个很小的 shell。读一行，按空格切成参数，fork 子进程去 exec 程序，等它们结束，再读下一行。
 *   - 命令名里没有 / 的，到 /bin 下找：ls 就是 /bin/ls；
 *   - 用 | 把几个命令接起来：每一段一个子进程，相邻两段之间一个管道，前一个的标准输出接到后一个的标准输入；
 *   - > 文件、>> 文件、< 文件：把这一段的标准输出写进文件（清空或者接在末尾），或者从文件读标准输入；
 *   - 最后一个参数是 & 的，在后台运行，不等它；每次读命令之前收走已经结束的后台程序；
 *   - 内建命令：exit 退出 shell（init 会再拉起一个），help 显示能用的命令。
 * Linux 0.11 的 init 运行的是磁盘上的 /bin/sh（当年的 bash），道理相同：pipe、fork、dup、exec、wait。
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
    *p = '\0';                      /* 参数满了：最后一个参数到这里为止，后面的都不要了 */
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

/*
 * 在子进程里处理这一段里的 >、>>、<：打开文件，接到 1 号或 0 号描述符上（先 close，再 dup 打开得到的描述符），
 * 再把这两个参数从 argv 里拿掉。打不开就报告、以 1 退出。
 */
static void redirect(char **argv)
{
    int out = 0;
    for (int i = 0; argv[i];) {
        int fd = strcmp(argv[i], ">") == 0 ? 1 : strcmp(argv[i], ">>") == 0 ? 1 : strcmp(argv[i], "<") == 0 ? 0 : -1;
        if (fd < 0) {
            argv[out++] = argv[i++];
            continue;
        }
        const char *file = argv[i + 1];
        int32_t flags = fd == 0 ? O_RDONLY : argv[i][1] == '>' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
        int32_t opened = file ? open(file, flags) : -1;
        if (opened < 0) {
            print("sh: ");
            print(file ? file : argv[i]);
            print(": cannot redirect\n");
            exit(1);
        }
        close(fd);
        dup(opened);
        close(opened);
        i += 2;
    }
    argv[out] = 0;
}

/* 在子进程里：命令名里没有 / 就到 /bin 下找，exec 它；exec 失败就报告、以 127 退出。 */
static void exec_command(char **argv)
{
    redirect(argv);
    if (!argv[0])
        exit(0);
    char path[64];
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
    execv(path, argv);
    print("sh: ");
    print(argv[0]);
    print(": not found\n");
    exit(127);
}

/*
 * 运行一行命令（可能有 |）。第 k 段的子进程：要是前面有管道，把它读的一头接到 0 号描述符；
 * 要是后面有管道，把写的一头接到 1 号描述符。接法都是先 close 再 dup：dup 返回最小的空闲描述符，
 * 正是刚关掉的那一个。管道多余的描述符都要关掉：只要还有人拿着写的一头，读的一方就永远等不到结束。
 */
static void run_line(char **argv, int argc, int background)
{
    int32_t pids[MAX_ARGS];
    const char *names[MAX_ARGS];
    int n = 0, start = 0;
    int32_t prev = -1;              /* 上一段管道读的一头 */

    for (int i = 0; i <= argc; i++) {
        if (i < argc && strcmp(argv[i], "|") != 0)
            continue;
        int last = i == argc;
        if (!last)
            argv[i] = 0;
        if (i == start) {
            print("sh: empty command\n");
            break;
        }
        int32_t p[2] = { -1, -1 };
        if (!last && pipe(p) < 0) {
            print("sh: pipe failed\n");
            break;
        }
        int32_t pid = fork();
        if (pid == 0) {
            if (prev >= 0) {
                close(0);
                dup(prev);
                close(prev);
            }
            if (!last) {
                close(1);
                dup(p[1]);
                close(p[0]);
                close(p[1]);
            }
            exec_command(&argv[start]);
        }
        if (prev >= 0)
            close(prev);
        prev = -1;
        if (!last) {
            close(p[1]);
            prev = p[0];
        }
        if (pid < 0) {
            print("sh: fork failed\n");
            break;
        }
        names[n] = argv[start];
        pids[n++] = pid;
        start = i + 1;
    }
    if (prev >= 0)
        close(prev);
    if (background) {
        if (n > 0) {
            print("[");
            print_num((uint32_t)pids[n - 1]);
            print("]\n");
        }
        return;
    }
    for (int k = 0; k < n; k++) {
        int32_t st = 0;
        waitpid(pids[k], &st, 0);
        report(names[k], st);
    }
}

int main(int sh_argc, char **sh_argv)
{
    (void)sh_argc, (void)sh_argv;
    char line[LINE_LEN];
    char *argv[MAX_ARGS + 1];
    int32_t st;

    print("shell on hda: try help, ls /bin, cat /hello.txt, ps\n");
    for (;;) {
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;                       /* 收走结束了的后台程序 */
        print("$ ");
        int32_t n = read(0, line, LINE_LEN - 1);
        if (n == 0)                 /* 空行上按了 Ctrl+D：像 exit 一样退出 */
            return 0;
        if (n < 0)
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
            print("commands: ls cat echo ps wc grep rm mkdir rmdir df hello check, letter X &, exit\n");
            print("pipes and files: ls /bin | wc, echo hi > /tmp/a, cat < /tmp/a\n");
            continue;
        }
        run_line(argv, argc, background);
    }
}
