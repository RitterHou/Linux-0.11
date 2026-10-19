/*
 * sh：一个很小的 shell。读一行，按空格切成参数，fork 子进程去 exec 程序，等它们结束，再读下一行。
 *   - 命令名里没有 / 的，到 /bin 下找：ls 就是 /bin/ls；
 *   - 用 | 把几个命令接起来：每一段一个子进程，相邻两段之间一个管道，前一个的标准输出接到后一个的标准输入；
 *   - > 文件、>> 文件、< 文件：把这一段的标准输出写进文件（清空或者接在末尾），或者从文件读标准输入；
 *     2> 文件、2>> 文件：标准错误写进文件；2>&1：标准错误也接到标准输出那里。都是子进程在 exec 之前用 dup2 接好。
 *     出错的消息都写到标准错误（2 号），所以管道里只有数据；
 *   - 最后一个参数是 & 的，在后台运行，不等它；每次读命令之前收走已经结束的后台程序；
 *   - 在终端上交互时，读命令用自己的行编辑（edit_line）：左右方向键挪光标，在光标处插入、退格，Home/End（Ctrl+A/E）
 *     到行首行尾，上下方向键翻出以前敲过的命令（最近 16 条）。为此读的时候把终端换成原始模式、关掉回显，
 *     每个键马上交来，回显由 shell 自己用转义序列画；读完换回规范模式再运行命令，程序看到的终端和原来一样。
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

/* ------------------------------------------------------------------ 行编辑和历史 */

#define HISTORY 16
static char history[HISTORY][LINE_LEN];     /* 敲过的命令，最近 16 条，环形放 */
static int history_count;                   /* 一共记过几条 */
static struct termios sane;                 /* 运行命令时终端该有的样子：规范模式、回显，开机时就是这样 */

static void put(const char *s, uint32_t n)
{
    if (n)
        write(1, s, n);
}

/* 光标往左挪 k 格：ESC [ k D。控制台和串口那头的终端都认得 */
static void cursor_left(uint32_t k)
{
    if (k == 0)
        return;
    char buf[16] = "\x1b[";
    uint32_t n = 2, digits = 1;
    for (uint32_t v = k; v >= 10; v /= 10)
        digits++;
    for (uint32_t i = digits; i > 0; i--, k /= 10)
        buf[n + i - 1] = (char)('0' + k % 10);
    n += digits;
    buf[n++] = 'D';
    put(buf, n);
}

/* 光标从 cur 起往后的部分重画：打印 line[cur..n)，擦掉行尾剩下的旧字，再挪回 cur */
static void redraw_tail(const char *line, uint32_t cur, uint32_t n)
{
    put(line + cur, n - cur);
    put("\x1b[K", 3);
    cursor_left(n - cur);
}

/* 换成另一行（翻历史时）：光标退到行首，画新的一行，擦掉旧的剩下的字 */
static void replace_line(char *line, uint32_t *cur, uint32_t *n, const char *text)
{
    cursor_left(*cur);
    *n = 0;
    while (text[*n] && *n < LINE_LEN - 1) {
        line[*n] = text[*n];
        (*n)++;
    }
    *cur = *n;
    put(line, *n);
    put("\x1b[K", 3);
}

/*
 * 在终端上读一行命令（提示符已经打好了），去掉换行放进 line，返回长度；在空行上按 Ctrl+D 返回 -1；Ctrl+C 返回 -EINTR。
 * 终端换成原始模式、关掉回显，一次读一个字节，自己处理：
 *   普通字符插在光标处；退格删掉光标前的一个；左右方向键挪光标；Home、End（Ctrl+A、Ctrl+E）到行首行尾；
 *   上下方向键翻历史：往上是更早的命令，往下回到较新的，最下面是正在敲的这一行；回车交出这一行，不空就记进历史。
 * 屏幕上的样子靠转义序列画：ESC [ n D 往左挪，ESC [ K 擦到行尾，往右挪是 ESC [ C。读完换回规范模式。
 * bash 用的 readline 库做的是同样的事，还多得多。
 */
static int32_t edit_line(char *line)
{
    struct termios raw = sane;
    raw.c_lflag &= ~(uint32_t)(ICANON | ECHO);
    ioctl(0, TCSETS, &raw);
    uint32_t n = 0, cur = 0;
    int browse = history_count;                 /* 正在看第几条历史；等于 history_count 是正在敲的这一行 */
    char typing[LINE_LEN];                      /* 翻历史之前正在敲的这一行 */
    int32_t result;

    for (;;) {
        char c, seq[2];
        int32_t got = read(0, &c, 1);
        if (got == -EINTR) {
            print("^C\n");                    /* 回显关着，终端没有回显 ^C */
            result = -EINTR;
            break;
        }
        if (got <= 0) {
            result = -1;
            break;
        }
        if (c == '\n') {
            print("\n");
            line[n] = '\0';
            result = (int32_t)n;
            break;
        }
        if (c == 0x04) {                        /* Ctrl+D：空行上是结束输入，否则不理 */
            if (n == 0) {
                result = -1;
                break;
            }
            continue;
        }
        if (c == 0x1B) {                        /* 方向键：ESC [ 再一个字母 */
            if (read(0, &seq[0], 1) != 1 || seq[0] != '[' || read(0, &seq[1], 1) != 1)
                continue;
            c = seq[1] == 'D' ? 0x02 : seq[1] == 'C' ? 0x06 : seq[1] == 'H' ? 0x01 : seq[1] == 'F' ? 0x05 :
                seq[1] == 'A' ? 0x10 : seq[1] == 'B' ? 0x0E : 0;     /* 换成 Emacs 的键：Ctrl+B、F、A、E、P、N */
        }
        if (c == 0x02 && cur > 0) {             /* 左 */
            cursor_left(1);
            cur--;
        } else if (c == 0x06 && cur < n) {      /* 右 */
            put("\x1b[C", 3);
            cur++;
        } else if (c == 0x01) {                 /* 行首 */
            cursor_left(cur);
            cur = 0;
        } else if (c == 0x05) {                 /* 行尾 */
            put(line + cur, n - cur);
            cur = n;
        } else if ((c == 0x10 && browse > 0 && browse > history_count - HISTORY) || (c == 0x0E && browse < history_count)) {
            if (browse == history_count) {      /* 离开正在敲的这一行：先存起来 */
                for (uint32_t i = 0; i < n; i++)
                    typing[i] = line[i];
                typing[n] = '\0';
            }
            browse += c == 0x10 ? -1 : 1;
            replace_line(line, &cur, &n, browse == history_count ? typing : history[browse % HISTORY]);
        } else if (c == '\b' && cur > 0) {      /* 退格：删掉光标前的一个 */
            for (uint32_t i = cur - 1; i + 1 < n; i++)
                line[i] = line[i + 1];
            n--, cur--;
            cursor_left(1);
            redraw_tail(line, cur, n);
        } else if ((uint8_t)c >= ' ' && c != 0x7F && n < LINE_LEN - 1) {     /* 普通字符：插在光标处 */
            for (uint32_t i = n; i > cur; i--)
                line[i] = line[i - 1];
            line[cur] = c;
            n++;
            put(&line[cur], 1);
            cur++;
            if (cur < n)
                redraw_tail(line, cur, n);
        }
    }
    ioctl(0, TCSETS, &sane);
    if (result > 0 && (history_count == 0 || strcmp(history[(history_count - 1) % HISTORY], line) != 0)) {
        for (uint32_t i = 0; i <= n; i++)       /* 和上一条一样的不重复记 */
            history[history_count % HISTORY][i] = line[i];
        history_count++;
    }
    return result;
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
    /* 标准输入是终端（TCGETS 成功）才用行编辑；管道、文件照旧一行行读。终端开机时是规范模式、回显，记下来 */
    int editing = interactive && ioctl(0, TCGETS, &sane) == 0;
    sane.c_lflag |= ICANON | ECHO;
    if (interactive) {
        signal(SIGTERM, SIG_IGN);   /* kill 0 发给这一组，也不该把 shell 自己结束；kill -9 才行 */
        print("shell on hda: try help, ls /bin, cat /hello.txt, ps\n");
    }
    for (;;) {
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;                       /* 收走结束了的后台程序 */
        if (interactive)
            print("$ ");
        int32_t n = editing ? edit_line(line) : read_line(in, line);
        if (n == -1)                /* 读完了：脚本读到末尾，或者在空行上按了 Ctrl+D，像 exit 一样退出 */
            return 0;
        if (n == LINE_TOO_LONG) {
            eprint("sh: line too long\n");
            continue;
        }
        if (n < 0)
            continue;               /* 在提示符上按了 Ctrl+C（-EINTR）：已经回显 ^C，重新打提示符 */
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
            print("files: ls cat echo wc grep sort rm ln mv mkdir rmdir chmod touch pwd\n");
            print("disks: df sync mount umount\n");
            print("tasks: ps kill sleep nice date letter hello check\n");
            print("users: id su login passwd chown\n");
            print("built in: cd [dir], exit [code], time command, help\n");
            print("try: ls /bin | wc, echo hi > /tmp/a, cat < /tmp/a, cd /etc, letter D &\n");
            continue;
        }
        run_line(argv, argc, background);
    }
}
