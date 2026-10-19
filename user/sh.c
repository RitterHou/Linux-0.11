/*
 * sh：一个很小的 shell。读一行，按空格切成参数，fork 子进程去 exec 程序，等它们结束，再读下一行。
 *   - 命令名里没有 / 的，到 /bin 下找：ls 就是 /bin/ls；
 *   - 用 | 把几个命令接起来：每一段一个子进程，相邻两段之间一个管道，前一个的标准输出接到后一个的标准输入；
 *   - > 文件、>> 文件、< 文件：把这一段的标准输出写进文件（清空或者接在末尾），或者从文件读标准输入；
 *     2> 文件、2>> 文件：标准错误写进文件；2>&1：标准错误也接到标准输出那里。都是子进程在 exec 之前用 dup2 接好。
 *     出错的消息都写到标准错误（2 号），所以管道里只有数据；
 *   - 最后一个参数是 & 的，在后台运行，不等它；每次读命令之前收走已经结束的后台程序；
 *   - Ctrl+C：终端给前台进程组发 SIGINT。shell 自己接住它（on_intr），被打断的 read 返回 -EINTR，
 *     重新打出提示符；被打断的 waitpid 也返回 -EINTR，接着等。前台的子进程在 exec 之前改回默认，就被结束；
 *     后台的子进程改成忽略，exec 之后也不变，所以 Ctrl+C 停不了后台程序，要用 kill。后台运行的脚本也是这样：
 *     运行它的 sh 一开始就忽略 SIGINT，它运行的每条命令也跟着忽略，不然这些命令和前台的 shell 在同一个进程组里，
 *     会被 Ctrl+C 结束。
 *   - 内建命令：cd 换当前目录（不带参数回到根目录），exit [退出码] 退出 shell（init 会再拉起一个），
 *     time 命令… 运行完报告用了多少时间，help 显示能用的命令。
 *     cd 必须是内建的：子进程换了目录，父进程的当前目录不会跟着变。
 *   - sh 脚本：从文件里一行行读命令，不打提示符，# 开头的行是注释；读完就退出。参数之间可以是空格或 Tab，
 *     行尾的回车不算；超过 127 个字节的行报错跳过。脚本的描述符设了 close-on-exec，它运行的命令拿不到。#! 开头、有执行权限的脚本
 *     可以直接运行：exec 发现 #!/bin/sh，就改成运行 sh 脚本（见 exec.c）。运行脚本的 sh 不接住 Ctrl+C，
 *     它和它正在运行的程序一起被结束。
 * Linux 0.11 的 init 运行的是磁盘上的 /bin/sh（当年的 bash），道理相同：pipe、fork、dup、exec、wait。
 */
#include "ulib.h"

#define LINE_LEN 128
#define MAX_ARGS 16

static int intr_ignored;            /* 启动时 SIGINT 就是忽略的（在后台运行的脚本）：它运行的命令也忽略 */

/* 空格、Tab 和回车都算分隔：Tab 缩进的行、Windows 写的每行末尾带回车的脚本也能运行。 */
static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* 原地把 line 按空白切开，argv 指向每一段，返回段数。 */
static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < MAX_ARGS) {
        while (is_blank(*p))
            *p++ = '\0';
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p && !is_blank(*p))
            p++;
    }
    *p = '\0';                      /* 参数满了：最后一个参数到这里为止，后面的都不要了 */
    argv[argc] = 0;
    return argc;
}

static void report(const char *name, int32_t st)
{
    if (st & 0x100) {               /* 被异常结束：0x100 加异常号 */
        eprint("sh: ");
        eprint(name);
        eprint(" killed by exception ");
        eprint_num((uint32_t)st & 0xFF);
        eprint("\n");
    } else if ((st & 0x200) && (st & 0xFF) != SIGINT && (st & 0xFF) != SIGPIPE) {
        /* 被信号结束。Ctrl+C 的 ^C 终端已经回显了；SIGPIPE 是后面的程序先走了，不算出错，和 bash 一样不报告 */
        eprint("sh: ");
        eprint(name);
        eprint(" killed by signal ");
        eprint_num((uint32_t)st & 0xFF);
        eprint("\n");
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
        const char *a = argv[i];
        if (strcmp(a, "2>&1") == 0) {   /* 标准错误也接到标准输出现在指向的地方 */
            dup2(1, 2);
            i++;
            continue;
        }
        int err = a[0] == '2' && a[1] == '>';   /* 2> 2>>：标准错误 */
        const char *op = err ? a + 1 : a;
        int fd = strcmp(op, ">") == 0 || strcmp(op, ">>") == 0 ? (err ? 2 : 1) : !err && strcmp(op, "<") == 0 ? 0 : -1;
        if (fd < 0) {
            argv[out++] = argv[i++];
            continue;
        }
        const char *file = argv[i + 1];
        int32_t flags = fd == 0 ? O_RDONLY : op[1] == '>' ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
        int32_t opened = file ? open(file, flags) : -1;
        if (opened < 0) {
            eprint("sh: ");
            eprint(file ? file : a);
            eprint(": cannot redirect\n");
            exit(1);
        }
        dup2(opened, fd);
        close(opened);
        i += 2;
    }
    argv[out] = 0;
}

/* 在子进程里：命令名里没有 / 就到 /bin 下找，exec 它；exec 失败就报告、以 127 退出。名字太长不截短：截短了可能是别的程序。 */
static void exec_command(char **argv)
{
    redirect(argv);
    if (!argv[0])
        exit(0);
    char path[128];                 /* 内核的路径最长 127 个字节 */
    uint32_t len = 0;
    int has_slash = 0;

    for (const char *p = argv[0]; *p; p++)
        has_slash |= *p == '/';
    if (!has_slash)
        for (const char *p = "/bin/"; *p; p++)
            path[len++] = *p;
    if (len + strlen(argv[0]) > sizeof(path) - 1) {
        eprint("sh: ");
        eprint(argv[0]);
        eprint(": name too long\n");
        exit(127);
    }
    for (const char *p = argv[0]; *p; p++)
        path[len++] = *p;
    path[len] = '\0';
    execv(path, argv);
    eprint("sh: ");
    eprint(argv[0]);
    eprint(": not found\n");
    exit(127);
}

/*
 * 运行一行命令（可能有 |）。第 k 段的子进程：要是前面有管道，把它读的一头接到 0 号描述符；
 * 要是后面有管道，把写的一头接到 1 号描述符，都用 dup2。早年没有 dup2 时是先 close 再 dup：dup 返回最小的空闲描述符，
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
            eprint("sh: empty command\n");
            break;
        }
        int32_t p[2] = { -1, -1 };
        if (!last && pipe(p) < 0) {
            eprint("sh: pipe failed\n");
            break;
        }
        int32_t pid = fork();
        if (pid == 0) {
            /*
             * 从 fork 到这里，子进程用的还是 shell 的 on_intr：这时按 Ctrl+C 只会打断一下，命令照样开始。
             * 没有 sigprocmask 这种先挡住信号的办法，这一小段避免不了。
             */
            signal(SIGINT, background || intr_ignored ? SIG_IGN : SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            if (prev >= 0) {        /* 前一段管道读的一头接到标准输入 */
                dup2(prev, 0);
                close(prev);
            }
            if (!last) {            /* 下一段管道写的一头接到标准输出 */
                dup2(p[1], 1);
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
            eprint("sh: fork failed\n");
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
        while (waitpid(pids[k], &st, 0) == -EINTR)
            ;                       /* 按了 Ctrl+C：shell 的 on_intr 处理完，接着等子进程 */
        report(names[k], st);
    }
}

/* SIGINT 的处理函数：什么也不用做。它的作用是打断 shell 正睡着的 read 或 waitpid，让它们返回 -EINTR。 */
static void on_intr(int sig)
{
    (void)sig;
}

#define LINE_TOO_LONG (-2)

/*
 * 从 fd 读一行，去掉换行放进 line，返回长度；读完了返回 -1；被信号打断返回 -EINTR，已经读到的半行丢掉；
 * 一行超过 127 个字节返回 LINE_TOO_LONG，整行都不要：截短了照样执行，可能删错文件。
 * 终端一次 read 正好是一行；文件一次读进来好几行，多出来的留在 buf 里给下一次。
 * 文件的最后一行没有换行也算一行，下一次再返回 -1。终端上在一行字后面按 Ctrl+D，read 交来的是不带换行的半行：
 * 先留着，接上后面敲的字，回车时一起算一行；接着再按一次 Ctrl+D（read 返回 0），这半行算一行，然后结束。
 * dash 这些不带行编辑的 shell 也是这样。
 */
static char buf[512];
static uint32_t buf_len, buf_pos;
static int at_end;

static int32_t read_line(int32_t fd, char *line)
{
    uint32_t n = 0;
    int too_long = 0;
    for (;;) {
        while (buf_pos < buf_len) {
            char c = buf[buf_pos++];
            if (c == '\n') {
                line[n] = '\0';
                return too_long ? LINE_TOO_LONG : (int32_t)n;
            }
            if (n < LINE_LEN - 1)
                line[n++] = c;
            else
                too_long = 1;
        }
        if (at_end) {
            line[n] = '\0';
            return too_long ? LINE_TOO_LONG : n ? (int32_t)n : -1;
        }
        int32_t got = read(fd, buf, sizeof(buf));
        if (got == -EINTR)
            return -EINTR;          /* Ctrl+C：攒着的半行也不要了，不能当成读完了去执行它 */
        if (got <= 0)
            at_end = 1;
        else
            buf_len = (uint32_t)got, buf_pos = 0;
    }
}

int main(int sh_argc, char **sh_argv)
{
    char line[LINE_LEN];
    char *argv[MAX_ARGS + 1];
    int32_t st, in = 0;
    int interactive = sh_argc < 2;
    struct stat script;

    if (!interactive && sh_argv[1][0] == '-') {     /* 不认识任何选项：#!/bin/sh -x 不能把 -x 当成脚本去打开 */
        eprint("sh: ");
        eprint(sh_argv[1]);
        eprint(": unknown option\n");
        return 2;
    }
    if (!interactive && (in = open(sh_argv[1], O_RDONLY)) < 0) {
        eprint("sh: ");
        eprint(sh_argv[1]);
        eprint(": cannot open\n");
        return 1;
    }
    if (!interactive && (stat(sh_argv[1], &script) < 0 || (script.mode & 0170000) == 0040000)) {
        eprint("sh: ");
        eprint(sh_argv[1]);
        eprint(": is a directory\n");     /* 目录也能 read，读出来的是目录项，不能当命令 */
        return 1;
    }
    if (!interactive)
        fcntl(in, F_SETFD, FD_CLOEXEC);  /* 脚本的描述符不留给它运行的命令：每个任务只有 8 个 */
    /* 先问一下 SIGINT 原来怎么处理：signal 返回原来的处理方式，问的时候设成忽略，下面马上改回来 */
    sighandler_t old_intr = signal(SIGINT, SIG_IGN);
    intr_ignored = old_intr == SIG_IGN;
    signal(SIGINT, interactive && !intr_ignored ? on_intr : old_intr);  /* Ctrl+C 结束的是前台程序，不是 shell 自己 */
    if (interactive) {
        signal(SIGTERM, SIG_IGN);   /* kill 0 发给这一组，也不该把 shell 自己结束；kill -9 才行 */
        print("shell on hda: try help, ls /bin, cat /hello.txt, ps\n");
    }
    for (;;) {
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;                       /* 收走结束了的后台程序 */
        if (interactive)
            print("$ ");
        int32_t n = read_line(in, line);
        if (n == -1)                /* 读完了：脚本读到末尾，或者在空行上按了 Ctrl+D，像 exit 一样退出 */
            return 0;
        if (n == LINE_TOO_LONG) {
            eprint("sh: line too long\n");
            continue;
        }
        if (n < 0)
            continue;               /* 在提示符上按了 Ctrl+C（-EINTR）：终端已经回显 ^C，重新打提示符 */
        int argc = split(line, argv);
        if (argc == 0 || argv[0][0] == '#')
            continue;               /* 空行和注释 */
        int background = strcmp(argv[argc - 1], "&") == 0;
        if (background) {
            argv[--argc] = 0;
            if (argc == 0)
                continue;
        }
        if (strcmp(argv[0], "exit") == 0) {     /* exit [退出码]：不给就是 0 */
            uint32_t code = 0;
            if (argc > 1 && !parse_uint(argv[1], 10, 255, &code)) {
                eprint("sh: exit: ");
                eprint(argv[1]);
                eprint(": bad number\n");
                continue;
            }
            return (int)code;
        }
        if (strcmp(argv[0], "cd") == 0) {
            const char *dir = argc > 1 ? argv[1] : "/";
            if (chdir(dir) < 0) {
                eprint("sh: cd: ");
                eprint(dir);
                eprint(": not a directory\n");
            }
            continue;
        }
        if (strcmp(argv[0], "time") == 0) {
            /* time 命令…：运行完在标准错误上报告过了多久、用了多少 CPU 时间（等到的子进程的 cutime、cstime 多了多少） */
            struct tms before, after;
            uint32_t start = times(&before);
            if (argc > 1)
                run_line(argv + 1, argc - 1, background);
            uint32_t end = times(&after);
            eprint("real ");
            write_ticks(2, end - start);
            eprint(" user ");
            write_ticks(2, after.tms_cutime - before.tms_cutime);
            eprint(" sys ");
            write_ticks(2, after.tms_cstime - before.tms_cstime);
            eprint("\n");
            continue;
        }
        if (strcmp(argv[0], "help") == 0) {
            print("files: ls cat echo wc grep sort rm ln mv mkdir rmdir chmod touch df sync pwd\n");
            print("tasks: ps kill sleep nice date letter hello check\n");
            print("users: id su login passwd chown\n");
            print("built in: cd [dir], exit [code], time command, help\n");
            print("try: ls /bin | wc, echo hi > /tmp/a, cat < /tmp/a, cd /etc, letter D &\n");
            continue;
        }
        run_line(argv, argc, background);
    }
}
