/*
 * sh：一个小 shell。读进一条命令（可能有好几行），分成词，解析成一棵语法树，再执行它。
 *   - 命令名里没有 / 的，到 /bin 下找：ls 就是 /bin/ls；
 *   - 用 | 把几个命令接起来：每一段一个子进程，相邻两段之间一个管道，前一个的标准输出接到后一个的标准输入；
 *   - > 文件、>> 文件、< 文件：把这一段的标准输出写进文件（清空或者接在末尾），或者从文件读标准输入；
 *     2> 文件、2>> 文件：标准错误写进文件；2>&1：标准错误也接到标准输出那里。都是子进程在 exec 之前用 dup2 接好。
 *     出错的消息都写到标准错误（2 号），所以管道里只有数据；
 *   - 命令后面跟 & 的，在后台运行，不等它；每次读命令之前收走已经结束的后台程序；
 *   - 引号：'…' 里的一切都照原样；"…" 里还会换掉 $ 开头的东西；引号外面的 \ 让下一个字符照原样（\$、\"、\ 和空格）；
 *   - 变量：名字=值 设一个变量（只在这个 shell 里，没有环境变量）；$名字、${名字} 换成它的值，没设过的是空的；
 *     $? 是上一条命令的退出码，$$ 是 shell 的任务号，$# 是参数个数，$0～$9 是脚本的名字和参数；
 *     $((表达式)) 算整数：+ - * / % 和括号，比较 < <= > >= == != 得 1 或 0，里面可以直接写变量名；
 *   - 命令列表：a ; b 先后运行，a && b 前一个成功（退出码 0）才运行后一个，a || b 前一个失败才运行；换行和 ; 一样；
 *   - if 列表; then 列表; [elif 列表; then 列表;] [else 列表;] fi：按条件列表的退出码选一支；
 *     while 列表; do 列表; done：条件成功就再执行一遍；for 名字 in 词…; do 列表; done：名字依次取每个词。
 *     这些可以写在一行里，也可以分成几行：交互时没写完就打出 > 接着读。Ctrl+C 结束正在运行的程序，也跳出循环；
 *   - 在终端上交互时，读命令用自己的行编辑（edit_line）：左右方向键挪光标，在光标处插入、退格，Home/End（Ctrl+A/E）
 *     到行首行尾，上下方向键翻出以前敲过的命令（最近 16 条）。为此读的时候把终端换成原始模式、关掉回显，
 *     每个键马上交来，回显由 shell 自己用转义序列画；读完换回规范模式再运行命令，程序看到的终端和原来一样。
 *   - 作业控制（交互、标准输入是终端时）：每条管道是一个作业，有自己的进程组（第一个子进程的号）。前台作业运行时，
 *     shell 把终端的前台进程组换成它（TIOCSPGRP），Ctrl+C、Ctrl+Z 就只发给它；作业结束或者停下了，再换回 shell 自己。
 *     Ctrl+Z 让前台作业停下，shell 记下它；jobs 列出作业，fg [%n] 让它到前台接着运行，bg [%n] 在后台接着运行，
 *     kill %n 发给作业的整个进程组。后台作业读终端会被停下（SIGTTIN），用 fg 才能读。shell 自己忽略 SIGTSTP 这几个信号。
 *   - Ctrl+C：终端给前台进程组发 SIGINT。在提示符上按，shell 自己接住它（on_intr），被打断的 read 返回 -EINTR，
 *     重新打出提示符。前台作业被 Ctrl+C 结束，正在运行的循环也不再往下转。
 *     没有作业控制的时候（脚本），命令都在 shell 的进程组里：后台的子进程改成忽略 SIGINT，exec 之后也不变，
 *     所以 Ctrl+C 停不了它们。后台运行的脚本也是这样：运行它的 sh 一开始就忽略 SIGINT，它运行的每条命令也跟着忽略。
 *   - 内建命令：cd 换当前目录（不带参数回到根目录），exit [退出码] 退出 shell（init 会再拉起一个），
 *     time 命令… 运行完报告用了多少时间，test 和 [ … ] 判断条件，true、false，help 显示能用的命令。
 *     cd 必须是内建的：子进程换了目录，父进程的当前目录不会跟着变。
 *   - sh 脚本 [参数…]：从文件里读命令，不打提示符，# 开头的是注释；读完就退出，退出码是最后一条命令的。
 *     参数之间可以是空格或 Tab，行尾的回车不算；超过 127 个字节的行报错跳过。脚本的描述符设了 close-on-exec，
 *     它运行的命令拿不到。#! 开头、有执行权限的脚本可以直接运行：exec 发现 #!/bin/sh，就改成运行 sh 脚本（见 exec.c）。
 *     运行脚本的 sh 不接住 Ctrl+C，它和它正在运行的程序一起被结束。
 * 没有的：命令替换 $(…)、通配符 *、环境变量、函数、here 文档，复合命令后面不能跟重定向。
 * Linux 0.11 的 init 运行的是磁盘上的 /bin/sh（当年的 bash），道理相同：pipe、fork、dup、exec、wait。
 */
#include "ulib.h"

#define LINE_LEN 128
#define MAX_ARGS 16
#define TEXT_MAX 2048               /* 一条命令（可能有好几行）最长这么多 */
#define MAX_TOKENS 512

static int intr_ignored;            /* 启动时 SIGINT 就是忽略的（在后台运行的脚本）：它运行的命令也忽略 */
static volatile int interrupted;    /* 这条命令运行时按过 Ctrl+C：循环不再往下转 */
static int last_status;             /* $? */
static int job_control;             /* 交互、标准输入是终端：每个作业一个进程组 */
static int32_t shell_pgid;          /* shell 自己的进程组：作业结束或停下以后，终端的前台换回它 */
static int32_t shell_pid;           /* $$：shell 自己的任务号。命令的词在 fork 之后的子进程里展开，不能在那时才 getpid() */

/* 作业表：后台运行的、Ctrl+Z 停下的作业 */
#define MAX_JOBS 8
struct job {
    int used;
    int32_t pgid;                   /* 作业的进程组 */
    int32_t pids[MAX_ARGS];         /* 还没收走的进程 */
    int npids;
    int stopped;
    char cmd[64];                   /* jobs 里显示的命令 */
};
static struct job jobs[MAX_JOBS];
static int pos_count;               /* $# */
static char **pos_args;             /* $0、$1… */

/* SIGINT 的处理函数：记一下，好让循环停下。它的主要作用是打断 shell 正睡着的 read 或 waitpid，让它们返回 -EINTR。 */
static void on_intr(int sig)
{
    (void)sig;
    interrupted = 1;
}

/* ------------------------------------------------------------------ 变量 */

#define MAX_VARS 32
static struct {
    char name[16];
    char value[LINE_LEN];
} vars[MAX_VARS];

static int name_char(char c, int first)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
}

static int same_name(const char *a, const char *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (a[i] != b[i])
            return 0;
    return a[len] == '\0';
}

/* 变量 name（长 len）的值，没设过的是空串 */
static const char *get_var(const char *name, uint32_t len)
{
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].name[0] && same_name(vars[i].name, name, len))
            return vars[i].value;
    return "";
}

static int set_var(const char *name, uint32_t len, const char *value)
{
    int slot = -1;
    for (int i = 0; i < MAX_VARS && slot < 0; i++)
        if (vars[i].name[0] && same_name(vars[i].name, name, len))
            slot = i;
    for (int i = 0; i < MAX_VARS && slot < 0; i++)
        if (!vars[i].name[0])
            slot = i;
    if (slot < 0 || len >= sizeof(vars[0].name) || strlen(value) >= sizeof(vars[0].value)) {
        eprint("sh: too many variables, or too long\n");
        return -1;
    }
    for (uint32_t i = 0; i < len; i++)
        vars[slot].name[i] = name[i];
    vars[slot].name[len] = '\0';
    uint32_t i = 0;
    for (; value[i]; i++)
        vars[slot].value[i] = value[i];
    vars[slot].value[i] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ 分词 */

/*
 * 把一条命令的文字切成词和操作符。词保留原样（连引号、反斜杠一起），执行时才展开（expand）：
 * 循环每转一圈都要重新换一遍变量。引号没配对返回 INCOMPLETE：还没写完，再读一行接上。
 */
enum { T_WORD, T_PIPE, T_OR, T_AMP, T_AND, T_SEMI, T_NL, T_IN, T_OUT, T_APPEND, T_ERR, T_ERR_APPEND, T_ERR_DUP, T_END };
#define INCOMPLETE (-1)
#define TOO_LONG   (-2)

struct token {
    int type;
    char *word;
};

static struct token toks[MAX_TOKENS];
static char words[TEXT_MAX * 2];    /* 词的原文，一个接一个，以 0 隔开 */

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';   /* 空格、Tab、回车都算分隔：Windows 写的脚本也能运行 */
}

static int is_operator(const char *p)
{
    return *p == '|' || *p == '&' || *p == ';' || *p == '<' || *p == '>' || *p == '\n';
}

static int tokenize(const char *text)
{
    int n = 0;
    uint32_t used = 0;
    const char *p = text;

    for (;;) {
        while (is_blank(*p))
            p++;
        if (*p == '#')                              /* 注释：到行尾 */
            while (*p && *p != '\n')
                p++;
        if (n == MAX_TOKENS - 1)
            return TOO_LONG;
        struct token *t = &toks[n];
        t->word = 0;
        if (*p == '\0') {
            t->type = T_END;
            return n + 1;
        }
        static const struct { const char *s; int type; } ops[] = {
            { "2>&1", T_ERR_DUP }, { "2>>", T_ERR_APPEND }, { "2>", T_ERR }, { "||", T_OR }, { "&&", T_AND },
            { ">>", T_APPEND }, { "|", T_PIPE }, { "&", T_AMP }, { ";", T_SEMI }, { "\n", T_NL }, { "<", T_IN }, { ">", T_OUT },
        };
        int matched = 0;
        for (uint32_t k = 0; k < sizeof(ops) / sizeof(ops[0]) && !matched; k++) {
            uint32_t len = strlen(ops[k].s);
            uint32_t i = 0;
            while (i < len && p[i] == ops[k].s[i])
                i++;
            if (i == len) {
                t->type = ops[k].type;
                p += len;
                matched = 1;
            }
        }
        if (matched) {
            n++;
            continue;
        }
        /* 一个词：到空白或操作符为止；引号里的、反斜杠后面的、$((…)) 里的都算在词里 */
        t->type = T_WORD;
        t->word = words + used;
        while (*p && !is_blank(*p) && !is_operator(p)) {
            const char *start = p;
            if (*p == '\\' && p[1] == '\n') {     /* 反斜杠加换行：接到下一行，两个都不要 */
                p += 2;
                continue;
            } else if (*p == '\\' && p[1]) {
                p += 2;
            } else if (*p == '\'' || *p == '"') {
                char q = *p++;
                while (*p && *p != q)
                    p += (q == '"' && *p == '\\' && p[1]) ? 2 : 1;
                if (!*p)
                    return INCOMPLETE;
                p++;
            } else if (p[0] == '$' && p[1] == '(' && p[2] == '(') {
                int depth = 0;
                for (p += 1; *p; p++) {
                    depth += *p == '(' ? 1 : *p == ')' ? -1 : 0;
                    if (depth == 0)
                        break;
                }
                if (!*p)
                    return INCOMPLETE;
                p++;
            } else {
                p++;
            }
            if (used + (uint32_t)(p - start) + 1 >= sizeof(words))
                return TOO_LONG;
            for (; start < p; start++)
                words[used++] = *start;
        }
        words[used++] = '\0';
        n++;
    }
}

/* ------------------------------------------------------------------ 语法树 */

enum { N_CMD, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_IF, N_WHILE, N_FOR, N_TIME };

struct redir {
    int type;                       /* T_IN、T_OUT 等 */
    char *word;                     /* 文件名（2>&1 没有） */
};

struct node {
    int kind;
    struct node *a, *b, *c;         /* SEQ、AND、OR、PIPE 的两边；IF 的条件、then、else；WHILE 的条件和循环体；
                                       FOR 的循环体在 b；BG、TIME 的命令在 a */
    char **words;                   /* CMD 的词；FOR 的变量名和要取的词 */
    int nwords;
    struct redir *redirs;
    int nredirs;
};

/* 语法树放在一块固定的内存里，一条命令执行完就整块清掉 */
static char arena[16384];
static uint32_t arena_used;

static void *alloc(uint32_t n)
{
    n = (n + 3) & ~3u;
    if (arena_used + n > sizeof(arena))
        return 0;
    void *p = arena + arena_used;
    arena_used += n;
    for (uint32_t i = 0; i < n; i++)
        ((char *)p)[i] = 0;
    return p;
}

static int tpos;                    /* 解析到第几个词 */
static int parse_state;             /* 0：好的；INCOMPLETE：还没写完；1：语法错 */

static struct node *mk(int kind, struct node *a, struct node *b)
{
    struct node *n = alloc(sizeof(struct node));
    if (!n) {
        parse_state = 1;
        return 0;
    }
    n->kind = kind;
    n->a = a;
    n->b = b;
    return n;
}

static int peek(void)
{
    return toks[tpos].type;
}

/* 现在这个词是不是保留字 w（if、then、fi……）：没有引号、一模一样才算 */
static int at_word(const char *w)
{
    return peek() == T_WORD && strcmp(toks[tpos].word, w) == 0;
}

static int at_terminator(void)
{
    static const char *ends[] = { "then", "elif", "else", "fi", "do", "done" };
    for (uint32_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++)
        if (at_word(ends[i]))
            return 1;
    return 0;
}

static void syntax_error(void)
{
    if (parse_state)
        return;
    if (peek() == T_END) {          /* 还没写完：再读一行 */
        parse_state = INCOMPLETE;
        return;
    }
    parse_state = 1;
    eprint("sh: syntax error near ");
    static const char *names[] = { "", "|", "||", "&", "&&", ";", "newline", "<", ">", ">>", "2>", "2>>", "2>&1", "end" };
    eprint(peek() == T_WORD ? toks[tpos].word : names[peek()]);
    eprint("\n");
}

static void expect(const char *w)
{
    if (at_word(w))
        tpos++;
    else
        syntax_error();
}

static void skip_newlines(void)
{
    while (peek() == T_NL)
        tpos++;
}

static struct node *parse_list(void);

/* 简单命令：词和重定向，到操作符、换行或者结尾为止 */
static struct node *parse_simple(void)
{
    char *w[MAX_TOKENS];
    struct redir r[8];
    int nw = 0, nr = 0;
    for (;;) {
        int t = peek();
        if (t == T_WORD) {
            w[nw++] = toks[tpos++].word;
        } else if (t >= T_IN && t <= T_ERR_DUP) {
            tpos++;
            if (nr == 8) {
                syntax_error();
                return 0;
            }
            r[nr].type = t;
            r[nr].word = 0;
            if (t != T_ERR_DUP) {
                if (peek() != T_WORD) {
                    syntax_error();
                    return 0;
                }
                r[nr].word = toks[tpos++].word;
            }
            nr++;
        } else {
            break;
        }
    }
    if (nw == 0 && nr == 0) {
        syntax_error();
        return 0;
    }
    struct node *n = mk(N_CMD, 0, 0);
    if (!n || !(n->words = alloc(sizeof(char *) * (uint32_t)(nw + 1))) || !(n->redirs = alloc(sizeof(struct redir) * (uint32_t)nr + 4))) {
        parse_state = 1;
        return 0;
    }
    for (int i = 0; i < nw; i++)
        n->words[i] = w[i];
    for (int i = 0; i < nr; i++)
        n->redirs[i] = r[i];
    n->nwords = nw;
    n->nredirs = nr;
    return n;
}

/* if 条件; then 列表; [elif …] [else 列表;] fi：elif 当成 else 里面再套一个 if，最里面那个吃掉 fi */
static struct node *parse_if(void)
{
    tpos++;                         /* if 或者 elif */
    struct node *cond = parse_list();
    expect("then");
    struct node *then = parse_list();
    struct node *n = mk(N_IF, cond, then);
    if (parse_state || !n)
        return 0;
    if (at_word("elif")) {
        n->c = parse_if();
        return parse_state ? 0 : n;
    }
    if (at_word("else")) {
        tpos++;
        n->c = parse_list();
    }
    expect("fi");
    return parse_state ? 0 : n;
}

static struct node *parse_command(void)
{
    if (at_word("if"))
        return parse_if();
    if (at_word("while")) {
        tpos++;
        struct node *cond = parse_list();
        expect("do");
        struct node *body = parse_list();
        expect("done");
        return parse_state ? 0 : mk(N_WHILE, cond, body);
    }
    if (at_word("for")) {
        tpos++;
        char *w[MAX_TOKENS];
        int nw = 0;
        if (peek() != T_WORD || !name_char(toks[tpos].word[0], 1)) {
            syntax_error();
            return 0;
        }
        w[nw++] = toks[tpos++].word;
        int have_in = at_word("in");
        if (have_in) {
            tpos++;
            while (peek() == T_WORD)
                w[nw++] = toks[tpos++].word;
        }
        if (peek() == T_SEMI || peek() == T_NL)
            tpos++;
        else if (have_in)
            syntax_error();
        skip_newlines();
        expect("do");
        struct node *body = parse_list();
        expect("done");
        struct node *n = parse_state ? 0 : mk(N_FOR, 0, body);
        if (!n || !(n->words = alloc(sizeof(char *) * (uint32_t)(nw + 1)))) {
            parse_state = parse_state ? parse_state : 1;
            return 0;
        }
        for (int i = 0; i < nw; i++)
            n->words[i] = w[i];
        n->nwords = have_in ? nw : -1;      /* 没有 in：取脚本的参数 $1 $2 … */
        return n;
    }
    if (at_terminator()) {
        syntax_error();
        return 0;
    }
    return parse_simple();
}

/* [time] 命令 | 命令 | … */
static struct node *parse_pipeline(void)
{
    int timed = at_word("time");
    if (timed)
        tpos++;
    if (timed && (peek() == T_NL || peek() == T_SEMI || peek() == T_END))
        return mk(N_TIME, 0, 0);    /* 单独一个 time：什么也不运行，报告三个 0 */
    struct node *n = parse_command();
    while (!parse_state && peek() == T_PIPE) {
        tpos++;
        skip_newlines();
        n = mk(N_PIPE, n, parse_command());
    }
    return timed && !parse_state ? mk(N_TIME, n, 0) : n;
}

static struct node *parse_and_or(void)
{
    struct node *n = parse_pipeline();
    while (!parse_state && (peek() == T_AND || peek() == T_OR)) {
        int kind = peek() == T_AND ? N_AND : N_OR;
        tpos++;
        skip_newlines();
        n = mk(kind, n, parse_pipeline());
    }
    return n;
}

/* 命令列表：用 ; & 换行隔开，到结尾或者 then、fi、do、done 这些词为止。空的列表是 0 */
static struct node *parse_list(void)
{
    struct node *list = 0;
    for (;;) {
        skip_newlines();
        if (parse_state || peek() == T_END || at_terminator())
            return list;
        struct node *n = parse_and_or();
        if (parse_state)
            return 0;
        if (peek() == T_AMP) {
            tpos++;
            n = mk(N_BG, n, 0);
        } else if (peek() == T_SEMI || peek() == T_NL) {
            tpos++;
        } else if (peek() != T_END && !at_terminator()) {
            syntax_error();
            return 0;
        }
        list = list ? mk(N_SEQ, list, n) : n;
    }
}

/* ------------------------------------------------------------------ 展开 */

/* $((…)) 里的整数表达式：比较 → 加减 → 乘除余 → 一元负号、括号、数、变量名 */
static const char *ap;
static int arith_bad;

static void arith_space(void)
{
    while (is_blank(*ap))
        ap++;
}

static int32_t arith_cmp(void);

static int32_t arith_atom(void)
{
    arith_space();
    if (*ap == '-') {
        ap++;
        return -arith_atom();
    }
    if (*ap == '(') {
        ap++;
        int32_t v = arith_cmp();
        arith_space();
        if (*ap == ')')
            ap++;
        else
            arith_bad = 1;
        return v;
    }
    if (*ap == '$')
        ap++;
    if (*ap >= '0' && *ap <= '9') {
        int32_t v = 0;
        while (*ap >= '0' && *ap <= '9')
            v = v * 10 + (*ap++ - '0');
        return v;
    }
    if (name_char(*ap, 1)) {
        const char *start = ap;
        while (name_char(*ap, 0))
            ap++;
        const char *s = get_var(start, (uint32_t)(ap - start));
        int neg = *s == '-';
        uint32_t v = 0;
        if (*s && !parse_uint(s + neg, 10, 0x7FFFFFFF, &v))
            arith_bad = 1;          /* 变量的值不是数 */
        return neg ? -(int32_t)v : (int32_t)v;
    }
    arith_bad = 1;
    return 0;
}

static int32_t arith_term(void)
{
    int32_t v = arith_atom();
    for (;;) {
        arith_space();
        char op = *ap;
        if (op != '*' && op != '/' && op != '%')
            return v;
        ap++;
        int32_t r = arith_atom();
        if (op != '*' && r == 0) {
            arith_bad = 2;          /* 除以 0 */
            return 0;
        }
        v = op == '*' ? v * r : op == '/' ? v / r : v % r;
    }
}

static int32_t arith_sum(void)
{
    int32_t v = arith_term();
    for (;;) {
        arith_space();
        if (*ap != '+' && *ap != '-')
            return v;
        char op = *ap++;
        int32_t r = arith_term();
        v = op == '+' ? v + r : v - r;
    }
}

static int32_t arith_cmp(void)
{
    int32_t v = arith_sum();
    for (;;) {
        arith_space();
        int op = 0;                 /* 1 < 2 <= 3 > 4 >= 5 == 6 != */
        if (ap[0] == '<')
            op = ap[1] == '=' ? 2 : 1;
        else if (ap[0] == '>')
            op = ap[1] == '=' ? 4 : 3;
        else if (ap[0] == '=' && ap[1] == '=')
            op = 5;
        else if (ap[0] == '!' && ap[1] == '=')
            op = 6;
        if (!op)
            return v;
        ap += (op == 1 || op == 3) ? 1 : 2;
        int32_t r = arith_sum();
        v = op == 1 ? v < r : op == 2 ? v <= r : op == 3 ? v > r : op == 4 ? v >= r : op == 5 ? v == r : v != r;
    }
}

/* 把 out 里第 *n 个位置起放上 s；放不下返回 -1 */
static int append(char *out, uint32_t *n, uint32_t size, const char *s)
{
    for (; *s; s++) {
        if (*n + 1 >= size)
            return -1;
        out[(*n)++] = *s;
    }
    return 0;
}

static const char *decimal(int32_t v, char *buf)
{
    int i = 11, neg = v < 0;
    uint32_t u = neg ? 0u - (uint32_t)v : (uint32_t)v;
    buf[i] = '\0';
    do
        buf[--i] = (char)('0' + u % 10);
    while ((u /= 10) != 0);
    if (neg)
        buf[--i] = '-';
    return buf + i;
}

/*
 * 换掉 *pp 处的 $ 开头的东西（*pp 指着 $），值接在 out 后面，*pp 挪到它后面。
 * 返回 0；放不下返回 -1；写错了、算错了（已经报告过）返回 -2。$ 后面不是认得的东西，就是一个普通的 $。
 */
static int expand_dollar(const char **pp, char *out, uint32_t *n, uint32_t size)
{
    const char *p = *pp + 1;
    char num[12];
    const char *value = 0;
    if (*p == '?') {
        value = decimal(last_status, num), p++;
    } else if (*p == '$') {
        value = decimal(shell_pid, num), p++;
    } else if (*p == '#') {
        value = decimal(pos_count, num), p++;
    } else if (*p >= '0' && *p <= '9') {
        int k = *p++ - '0';
        value = k <= pos_count ? pos_args[k] : "";
    } else if (p[0] == '(' && p[1] == '(') {
        /* $((…))：到配对的 )) 为止 */
        const char *start = p + 2;
        int depth = 0;
        for (; *p; p++) {
            depth += *p == '(' ? 1 : *p == ')' ? -1 : 0;
            if (depth == 0)
                break;
        }
        char expr[LINE_LEN];
        uint32_t len = (uint32_t)(p - 1 - start);
        if (len >= sizeof(expr))
            return -1;
        for (uint32_t i = 0; i < len; i++)
            expr[i] = start[i];
        expr[len] = '\0';
        ap = expr;
        arith_bad = 0;
        int32_t v = arith_cmp();
        arith_space();
        if (arith_bad || *ap) {
            eprint(arith_bad == 2 ? "sh: division by zero\n" : "sh: bad arithmetic expression\n");
            return -2;
        }
        value = decimal(v, num);
        p++;
    } else if (*p == '{') {
        const char *start = ++p;
        while (name_char(*p, p == start))
            p++;
        if (*p != '}' || p == start) {
            eprint("sh: bad substitution\n");
            return -2;
        }
        value = get_var(start, (uint32_t)(p - start));
        p++;
    } else if (name_char(*p, 1)) {
        const char *start = p;
        while (name_char(*p, 0))
            p++;
        value = get_var(start, (uint32_t)(p - start));
    } else {
        value = "$";
    }
    *pp = p;
    return append(out, n, size, value);
}

/*
 * 展开一个词：去掉引号，处理反斜杠，换掉 $ 开头的东西，结果放进 out。*quoted 说明这个词里有没有引号：
 * 没有引号、展开以后又是空的词，算没有这个词（echo $empty x 只有一个参数），有引号的空词 "" 还是一个参数。
 * 不做按空白再切开（$x 的值里有空格也还是一个参数），也没有通配符。太长返回 -1，写错了（已经报告过）返回 -2。
 */
static int expand(const char *raw, char *out, uint32_t size, int *quoted)
{
    uint32_t n = 0;
    *quoted = 0;
    for (const char *p = raw; *p;) {
        if (*p == '\\' && p[1]) {
            char s[2] = { p[1], 0 };
            if (append(out, &n, size, s) < 0)
                return -1;
            p += 2;
        } else if (*p == '\'') {
            *quoted = 1;
            for (p++; *p != '\''; p++) {
                char s[2] = { *p, 0 };
                if (append(out, &n, size, s) < 0)
                    return -1;
            }
            p++;
        } else if (*p == '"') {
            *quoted = 1;
            for (p++; *p != '"';) {
                if (*p == '$') {
                    int r = expand_dollar(&p, out, &n, size);
                    if (r < 0)
                        return r;
                    continue;
                }
                if (*p == '\\' && (p[1] == '$' || p[1] == '"' || p[1] == '\\'))
                    p++;
                char s[2] = { *p++, 0 };
                if (append(out, &n, size, s) < 0)
                    return -1;
            }
            p++;
        } else if (*p == '$') {
            int r = expand_dollar(&p, out, &n, size);
            if (r < 0)
                return r;
        } else {
            char s[2] = { *p++, 0 };
            if (append(out, &n, size, s) < 0)
                return -1;
        }
    }
    out[n] = '\0';
    return (int)n;
}

/* ------------------------------------------------------------------ 执行 */

struct argv_buf {
    char text[1024];
    char *argv[MAX_ARGS + 1];
    int argc;
};

/* 展开命令的每个词放进 b：出错（展开出错、参数太多）返回 -1 */
static int expand_args(struct node *n, struct argv_buf *b)
{
    uint32_t used = 0;
    b->argc = 0;
    for (int i = 0; i < n->nwords; i++) {
        int quoted;
        int len = expand(n->words[i], b->text + used, sizeof(b->text) - used, &quoted);
        if (len < 0) {
            if (len == -1)
                eprint("sh: words too long\n");
            return -1;
        }
        if (len == 0 && !quoted)
            continue;
        if (b->argc == MAX_ARGS) {
            eprint("sh: too many arguments\n");
            return -1;
        }
        b->argv[b->argc++] = b->text + used;
        used += (uint32_t)len + 1;
    }
    b->argv[b->argc] = 0;
    return 0;
}

/* 退出状态换成退出码：正常退出就是 exit 的数；被信号结束是 128 加信号编号；被异常结束是 128 加异常号 */
static int status_code(int32_t st)
{
    return (st & 0x300) ? 128 + (st & 0xFF) : st & 0xFF;
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

/* 在子进程里接好重定向：打开文件，用 dup2 接到 0、1、2 号描述符上。打不开就报告、以 1 退出。 */
static void redirect(struct node *n)
{
    for (int i = 0; i < n->nredirs; i++) {
        struct redir *r = &n->redirs[i];
        if (r->type == T_ERR_DUP) {     /* 标准错误也接到标准输出现在指向的地方 */
            dup2(1, 2);
            continue;
        }
        char file[LINE_LEN];
        int quoted;
        int fd = r->type == T_IN ? 0 : (r->type == T_ERR || r->type == T_ERR_APPEND) ? 2 : 1;
        int32_t flags = r->type == T_IN ? O_RDONLY
                      : (r->type == T_APPEND || r->type == T_ERR_APPEND) ? (O_WRONLY | O_CREAT | O_APPEND)
                      : (O_WRONLY | O_CREAT | O_TRUNC);
        int32_t opened = expand(r->word, file, sizeof(file), &quoted) < 0 ? -1 : open(file, flags);
        if (opened < 0) {
            eprint("sh: ");
            eprint(file);
            eprint(": cannot redirect\n");
            exit(1);
        }
        dup2(opened, fd);
        close(opened);
    }
}

static int builtin(struct argv_buf *b, int *status);
static int run(struct node *n);

/* 在子进程里运行一个简单命令：接好重定向，内建命令就地运行，别的到 /bin 下找来 exec；exec 失败就报告、以 127 退出。 */
static void exec_simple(struct node *n)
{
    struct argv_buf b;
    redirect(n);
    if (expand_args(n, &b) < 0)
        exit(1);
    int status;
    if (b.argc == 0)
        exit(0);
    if (builtin(&b, &status))
        exit(status);
    char path[128];                 /* 内核的路径最长 127 个字节 */
    uint32_t len = 0;
    int has_slash = 0;
    for (const char *p = b.argv[0]; *p; p++)
        has_slash |= *p == '/';
    if (!has_slash)
        for (const char *p = "/bin/"; *p; p++)
            path[len++] = *p;
    if (len + strlen(b.argv[0]) > sizeof(path) - 1) {   /* 名字太长不截短：截短了可能是别的程序 */
        eprint("sh: ");
        eprint(b.argv[0]);
        eprint(": name too long\n");
        exit(127);
    }
    for (const char *p = b.argv[0]; *p; p++)
        path[len++] = *p;
    path[len] = '\0';
    execv(path, b.argv);
    eprint("sh: ");
    eprint(b.argv[0]);
    eprint(": not found\n");
    exit(127);
}

/* ------------------------------------------------------------------ 作业 */

static void add_text(char *out, uint32_t size, const char *text)
{
    uint32_t len = strlen(out);
    for (; *text && len + 1 < size; text++)
        out[len++] = *text;
    out[len] = '\0';
}

/* 把命令写成一行字给 jobs 显示：简单命令是它的词，管道用 | 连起来，复合命令只写开头的保留字 */
static void describe(struct node *n, char *out, uint32_t size)
{
    if (n->kind == N_PIPE) {
        describe(n->a, out, size);
        add_text(out, size, " | ");
        describe(n->b, out, size);
    } else if (n->kind == N_CMD) {
        for (int i = 0; i < n->nwords; i++) {
            if (i)
                add_text(out, size, " ");
            add_text(out, size, n->words[i]);
        }
    } else {
        add_text(out, size, n->kind == N_IF ? "if ..." : n->kind == N_WHILE ? "while ..." : n->kind == N_FOR ? "for ..." : "...");
    }
}

static void print_job(int k, const char *state)
{
    print("[");
    print_num((uint32_t)k + 1);
    print("] ");
    print(state);
    print(jobs[k].cmd);
    print("\n");
}

/* 收走或者停下了的进程 pid：找到它属于的作业，更新它；作业里的进程都收走了，作业就没了 */
static void job_update(int32_t pid, int32_t st)
{
    for (int k = 0; k < MAX_JOBS; k++) {
        struct job *j = &jobs[k];
        for (int i = 0; j->used && i < j->npids; i++) {
            if (j->pids[i] != pid)
                continue;
            if (st & 0x400) {
                j->stopped = 1;
                return;
            }
            j->pids[i] = j->pids[--j->npids];
            if (j->npids == 0)
                j->used = 0;
            return;
        }
    }
}

/*
 * 收走结束了的后台程序；读命令之前和 jobs、fg、bg、kill 之前都收一遍。作业是停着还是在运行，问内核（taskinfo）：
 * 有一个进程停着就算停着。别人用 kill -18 让它接着运行了，shell 也知道。
 */
static void reap_jobs(void)
{
    int32_t pid, st;
    struct taskinfo info;
    while ((pid = waitpid(-1, &st, WNOHANG | (job_control ? WUNTRACED : 0))) > 0)
        job_update(pid, st);
    for (int k = 0; k < MAX_JOBS; k++) {
        if (!jobs[k].used)
            continue;
        jobs[k].stopped = 0;
        for (int i = 0; i < jobs[k].npids; i++)
            if (taskinfo(jobs[k].pids[i], &info) == 0 && info.state == 6)
                jobs[k].stopped = 1;
    }
}

/* 记下一个作业，返回它在表里的位置；满了返回 -1 */
static int add_job(int32_t pgid, const int32_t *pids, int npids, int stopped, struct node *n)
{
    for (int k = 0; k < MAX_JOBS; k++) {
        if (jobs[k].used)
            continue;
        struct job *j = &jobs[k];
        j->used = 1;
        j->pgid = pgid;
        j->npids = npids;
        for (int i = 0; i < npids; i++)
            j->pids[i] = pids[i];
        j->stopped = stopped;
        j->cmd[0] = '\0';
        describe(n, j->cmd, sizeof(j->cmd));
        return k;
    }
    eprint("sh: too many jobs\n");
    return -1;
}

/* 终端的前台换成进程组 pgid（tcsetpgrp）：标准输入就是 shell 的终端 */
static void set_foreground(int32_t pgid)
{
    if (job_control)
        ioctl(0, TIOCSPGRP, &pgid);
}

/*
 * 等前台作业：等 pids 里的每一个进程结束或者停下（WUNTRACED）。有一个停下了，整个作业就算停下了：
 * 还没收走的进程都记进作业表（slot 是它原来在表里的位置，新的作业是 -1），打印 [n] stopped …，
 * 正在运行的循环也不再往下转；被 Ctrl+C 结束了也一样不再往下转。names 是报告时用的名字（可以是 0）。
 * 等完把终端的前台换回 shell 自己。返回退出码：最后一个进程的，停下的是 128 加信号编号。
 */
static int wait_job(int32_t pgid, int32_t *pids, const char **names, int npids, struct node *n, int slot)
{
    int32_t st = 0;
    for (int k = 0; k < npids; k++) {
        st = 0;
        while (waitpid(pids[k], &st, job_control ? WUNTRACED : 0) == -EINTR)
            ;                       /* 按了 Ctrl+C：shell 的 on_intr 处理完，接着等子进程 */
        if (st & 0x400) {           /* 停下了：这一个和后面还没等的都留在作业里 */
            if (slot < 0) {
                slot = add_job(pgid, pids + k, npids - k, 1, n);
            } else {
                jobs[slot].npids = npids - k;
                for (int i = 0; i < npids - k; i++)
                    jobs[slot].pids[i] = pids[k + i];
                jobs[slot].stopped = 1;
            }
            set_foreground(shell_pgid);
            if (slot >= 0)
                print_job(slot, "stopped ");
            interrupted = 1;
            return status_code(st);
        }
        if ((st & 0x200) && (st & 0xFF) == SIGINT)
            interrupted = 1;        /* 被 Ctrl+C 结束：作业有自己的进程组，shell 没收到 SIGINT，从这里知道 */
        report(names ? names[k] : slot >= 0 ? jobs[slot].cmd : "sh", st);
    }
    if (slot >= 0)
        jobs[slot].used = 0;
    set_foreground(shell_pgid);
    return status_code(st);
}

/* fg、bg 的参数：%n 或者 n 是第 n 个作业，不给就是最后一个 */
static int find_job(int argc, char **argv)
{
    int k = -1;
    if (argc > 1) {
        uint32_t num;
        const char *s = argv[1][0] == '%' ? argv[1] + 1 : argv[1];
        if (parse_uint(s, 10, MAX_JOBS, &num) && num >= 1 && jobs[num - 1].used)
            k = (int)num - 1;
    } else {
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used)
                k = i;
    }
    if (k < 0) {
        eprint("sh: ");
        eprint(argv[0]);
        eprint(": no such job\n");
    }
    return k;
}

/*
 * 运行一条管道（只有一段也算）：每一段 fork 一个子进程。第 k 段的子进程：要是前面有管道，把它读的一头接到 0 号描述符；
 * 要是后面有管道，把写的一头接到 1 号描述符，都用 dup2。管道多余的描述符都要关掉：只要还有人拿着写的一头，
 * 读的一方就永远等不到结束。简单命令在子进程里 exec，if、while 这些复合命令由子进程里的 shell 自己执行完再退出。
 * 有作业控制时，这些子进程放进一个新的进程组（第一个子进程的号），前台的作业运行时终端的前台就是这一组。
 * shell 和子进程都调用 setpgid、都设前台：谁先运行都一样，子进程不会在进组之前就去读终端。
 * 后台运行的不等，打印最后一段的任务号；前台的等每一段结束，退出码是最后一段的。
 */
static int run_pipeline(struct node *n, int background)
{
    struct node *seg[MAX_ARGS];
    struct node *whole = n;         /* 整条管道，jobs 里显示它；下面拆完以后 n 是第一段 */
    int nseg = 0;
    for (; n->kind == N_PIPE && nseg < MAX_ARGS - 1; n = n->a)
        seg[nseg++] = n->b;
    seg[nseg++] = n;
    int32_t pids[MAX_ARGS];
    const char *names[MAX_ARGS];
    int32_t prev = -1;              /* 上一段管道读的一头 */
    int32_t pgid = 0;
    int started = 0;
    for (int k = nseg - 1; k >= 0; k--) {       /* 拆的时候是从后往前拆的 */
        int last = k == 0;
        int32_t p[2] = { -1, -1 };
        if (!last && pipe(p) < 0) {
            eprint("sh: pipe failed\n");
            break;
        }
        int32_t pid = fork();
        if (pid == 0) {
            if (job_control) {
                setpgid(0, pgid);   /* 第一个子进程 pgid 是 0：新开一组，组号是它自己 */
                if (!background) {
                    int32_t g = getpgrp();
                    ioctl(0, TIOCSPGRP, &g);
                }
                signal(SIGTSTP, SIG_DFL);
                signal(SIGTTIN, SIG_DFL);
                signal(SIGTTOU, SIG_DFL);
            }
            /*
             * 从 fork 到这里，子进程用的还是 shell 的 on_intr：这时按 Ctrl+C 只会打断一下，命令照样开始。
             * 没有 sigprocmask 这种先挡住信号的办法，这一小段避免不了。有作业控制时后台作业不在前台的组里，
             * Ctrl+C 本来就到不了它，所以不用忽略，fg 到前台以后 Ctrl+C 照样能结束它。
             */
            signal(SIGINT, !job_control && (background || intr_ignored) ? SIG_IGN : SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            if (background && !job_control)
                intr_ignored = 1;   /* 后台的复合命令：它运行的命令也忽略 Ctrl+C */
            job_control = 0;        /* 子进程里的 shell（复合命令）不再管作业 */
            if (prev >= 0) {        /* 前一段管道读的一头接到标准输入 */
                dup2(prev, 0);
                close(prev);
            }
            if (!last) {            /* 下一段管道写的一头接到标准输出 */
                dup2(p[1], 1);
                close(p[0]);
                close(p[1]);
            }
            if (seg[k]->kind == N_CMD)
                exec_simple(seg[k]);
            exit(run(seg[k]));
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
        if (job_control) {
            setpgid(pid, pgid ? pgid : pid);
            if (!pgid && !background)
                set_foreground(pid);
        }
        if (!pgid)
            pgid = pid;
        names[started] = seg[k]->kind == N_CMD && seg[k]->nwords ? seg[k]->words[0] : "sh";
        pids[started++] = pid;
    }
    if (prev >= 0)
        close(prev);
    if (started == 0) {
        set_foreground(shell_pgid);
        return 1;
    }
    if (background) {
        if (job_control)
            add_job(pgid, pids, started, 0, whole);
        print("[");
        print_num((uint32_t)pids[started - 1]);
        print("]\n");
        return 0;
    }
    return wait_job(pgid, pids, names, started, whole, -1);
}

/* 词是不是 名字=值 */
static int is_assignment(const char *w)
{
    if (!name_char(w[0], 1))
        return 0;
    const char *p = w;
    while (name_char(*p, 0))
        p++;
    return *p == '=';
}

/* 简单命令在 shell 里：全是 名字=值 就设变量；内建命令（没有重定向的）就地运行；别的 fork 去运行 */
static int run_simple(struct node *n)
{
    int k = 0;
    while (k < n->nwords && is_assignment(n->words[k]))
        k++;
    for (int i = 0; i < k; i++) {
        char value[LINE_LEN];
        int quoted;
        const char *w = n->words[i], *eq = w;
        while (*eq != '=')
            eq++;
        if (expand(eq + 1, value, sizeof(value), &quoted) < 0 || set_var(w, (uint32_t)(eq - w), value) < 0)
            return 1;
    }
    if (k == n->nwords && n->nredirs == 0)
        return 0;
    struct node rest = *n;          /* 名字=值 后面还有命令：变量已经设上了（没有环境变量，只在 shell 里） */
    rest.words += k;
    rest.nwords -= k;
    if (n->nredirs == 0) {
        struct argv_buf b;
        int status;
        if (expand_args(&rest, &b) < 0)
            return 1;
        if (b.argc == 0)
            return 0;
        if (builtin(&b, &status))
            return status;
    }
    return run_pipeline(&rest, 0);
}

static int run(struct node *n)
{
    if (!n)
        return 0;
    switch (n->kind) {
    case N_SEQ:
        run(n->a);
        return interrupted ? last_status : run(n->b);
    case N_AND:
        last_status = run(n->a);
        return last_status == 0 && !interrupted ? run(n->b) : last_status;
    case N_OR:
        last_status = run(n->a);
        return last_status != 0 && !interrupted ? run(n->b) : last_status;
    case N_BG:
        last_status = run_pipeline(n->a, 1);
        return last_status;
    case N_PIPE:
        last_status = run_pipeline(n, 0);
        return last_status;
    case N_CMD:
        last_status = run_simple(n);
        return last_status;
    case N_IF:
        if (run(n->a) == 0 && !interrupted)
            return last_status = run(n->b);
        return last_status = interrupted ? last_status : n->c ? run(n->c) : 0;
    case N_WHILE: {
        int status = 0;
        while (!interrupted && run(n->a) == 0 && !interrupted)
            status = run(n->b);
        return last_status = status;
    }
    case N_FOR: {
        int status = 0;
        int count = n->nwords < 0 ? pos_count : n->nwords - 1;
        const char *name = n->words[0];
        for (int i = 0; i < count && !interrupted; i++) {
            char value[LINE_LEN];
            int quoted;
            if (n->nwords < 0) {
                if (set_var(name, strlen(name), pos_args[i + 1]) < 0)
                    break;
            } else if (expand(n->words[i + 1], value, sizeof(value), &quoted) < 0 || set_var(name, strlen(name), value) < 0) {
                status = 1;
                break;
            }
            status = run(n->b);
        }
        return last_status = status;
    }
    case N_TIME: {
        /* time 命令…：运行完在标准错误上报告过了多久、用了多少 CPU 时间（等到的子进程的 cutime、cstime 多了多少） */
        struct tms before, after;
        uint32_t start = times(&before);
        int status = run(n->a);
        uint32_t end = times(&after);
        eprint("real ");
        write_ticks(2, end - start);
        eprint(" user ");
        write_ticks(2, after.tms_cutime - before.tms_cutime);
        eprint(" sys ");
        write_ticks(2, after.tms_cstime - before.tms_cstime);
        eprint("\n");
        return last_status = status;
    }
    }
    return 1;
}

/* ------------------------------------------------------------------ 内建命令 */

static int to_int(const char *s, int32_t *v)
{
    int neg = *s == '-';
    uint32_t u;
    if (!parse_uint(s + neg, 10, 0x7FFFFFFF, &u))
        return 0;
    *v = neg ? -(int32_t)u : (int32_t)u;
    return 1;
}

/*
 * test 表达式、[ 表达式 ]：成立返回 0，不成立 1，写错了 2。认得：
 *   字符串：-n s（不空）、-z s（空）、s1 = s2、s1 != s2、只有一个 s（不空就成立）
 *   整数：n1 -eq -ne -lt -le -gt -ge n2
 *   文件：-e 存在、-f 普通文件、-d 目录、-r -w -x 当前用户能读、能写、能执行（按权限位估计）
 *   ! 表达式：反过来
 */
static int test_error(const char *why)
{
    eprint("sh: test: ");
    eprint(why);
    eprint("\n");
    return 2;
}

static int test(int argc, char **argv)
{
    if (argc > 0 && strcmp(argv[0], "!") == 0) {
        int r = test(argc - 1, argv + 1);
        return r == 2 ? 2 : !r;
    }
    if (argc == 0)
        return 1;
    if (argc == 1)
        return argv[0][0] ? 0 : 1;
    if (argc == 2) {
        const char *op = argv[0], *s = argv[1];
        struct stat st;
        if (strcmp(op, "-n") == 0)
            return s[0] ? 0 : 1;
        if (strcmp(op, "-z") == 0)
            return s[0] ? 1 : 0;
        if (op[0] == '-' && op[2] == '\0' && (op[1] == 'e' || op[1] == 'f' || op[1] == 'd' || op[1] == 'r' || op[1] == 'w' || op[1] == 'x')) {
            if (stat(s, &st) < 0)
                return 1;
            uint32_t type = st.mode & 0170000, euid = (uint32_t)geteuid();
            uint32_t bits = euid == 0 ? 7 : euid == st.uid ? (st.mode >> 6) & 7 : (uint32_t)getegid() == st.gid ? (st.mode >> 3) & 7 : st.mode & 7;
            if (euid == 0 && !(st.mode & 0111) && type != 0040000)
                bits &= 6;          /* root 也执行不了一个执行位都没有的文件 */
            switch (op[1]) {
            case 'e': return 0;
            case 'f': return type == 0100000 ? 0 : 1;
            case 'd': return type == 0040000 ? 0 : 1;
            case 'r': return (bits & 4) ? 0 : 1;
            case 'w': return (bits & 2) ? 0 : 1;
            default: return (bits & 1) ? 0 : 1;
            }
        }
        return test_error("bad expression");
    }
    if (argc == 3) {
        const char *a = argv[0], *op = argv[1], *b = argv[2];
        if (strcmp(op, "=") == 0)
            return strcmp(a, b) == 0 ? 0 : 1;
        if (strcmp(op, "!=") == 0)
            return strcmp(a, b) != 0 ? 0 : 1;
        static const char *ops[] = { "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
        for (int i = 0; i < 6; i++) {
            if (strcmp(op, ops[i]) != 0)
                continue;
            int32_t x, y;
            if (!to_int(a, &x) || !to_int(b, &y))
                return test_error("integer expected");
            int r = i == 0 ? x == y : i == 1 ? x != y : i == 2 ? x < y : i == 3 ? x <= y : i == 4 ? x > y : x >= y;
            return r ? 0 : 1;
        }
    }
    return test_error("bad expression");
}

/* argv 是内建命令就运行它，*status 是退出码，返回 1；不是返回 0 */
static int builtin(struct argv_buf *b, int *status)
{
    int argc = b->argc;
    char **argv = b->argv;
    const char *name = argv[0];
    *status = 0;
    if (strcmp(name, "exit") == 0) {        /* exit [退出码]：不给就是上一条命令的 */
        uint32_t code = (uint32_t)last_status;
        if (argc > 1 && !parse_uint(argv[1], 10, 255, &code)) {
            eprint("sh: exit: ");
            eprint(argv[1]);
            eprint(": bad number\n");
            *status = 2;
            return 1;
        }
        exit((int)code);
    }
    if (strcmp(name, "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : "/";
        if (chdir(dir) < 0) {
            eprint("sh: cd: ");
            eprint(dir);
            eprint(": not a directory\n");
            *status = 1;
        }
        return 1;
    }
    if (strcmp(name, "true") == 0 || strcmp(name, ":") == 0)
        return 1;
    if (strcmp(name, "false") == 0) {
        *status = 1;
        return 1;
    }
    if (strcmp(name, "test") == 0 || strcmp(name, "[") == 0) {
        if (name[0] == '[') {
            if (strcmp(argv[argc - 1], "]") != 0) {
                eprint("sh: [: missing ]\n");
                *status = 2;
                return 1;
            }
            argc--;
        }
        *status = test(argc - 1, argv + 1);
        return 1;
    }
    for (int i = 1; strcmp(name, "kill") == 0 && job_control && i < argc; i++) {
        if (argv[i][0] != '%')
            continue;
        /*
         * kill [-信号] %n…：发给作业的整个进程组。作业停着的话再发一个 SIGCONT，不然信号要等它接着运行才处理。
         * bash 的 kill 也是内建的，也这样做。参数里没有 %n 的，照常运行 /bin/kill。
         */
        uint32_t sig = SIGTERM;
        int first = 1;
        if (argv[1][0] == '-' && !parse_uint(argv[1] + 1, 10, 32, &sig)) {
            eprint("sh: kill: bad signal\n");
            *status = 2;
            return 1;
        }
        if (argv[1][0] == '-')
            first = 2;
        reap_jobs();
        for (int a = first; a < argc; a++) {
            uint32_t num;
            if (argv[a][0] != '%' || !parse_uint(argv[a] + 1, 10, MAX_JOBS, &num) || num < 1 || !jobs[num - 1].used) {
                eprint("sh: kill: ");
                eprint(argv[a]);
                eprint(": no such job\n");
                *status = 1;
                continue;
            }
            kill(-jobs[num - 1].pgid, (int32_t)sig);
            if (jobs[num - 1].stopped && sig != SIGKILL && sig != SIGCONT)
                kill(-jobs[num - 1].pgid, SIGCONT);
        }
        return 1;
    }
    if (strcmp(name, "jobs") == 0) {        /* 列出作业：[n] 状态 命令 */
        reap_jobs();
        for (int k = 0; k < MAX_JOBS; k++)
            if (jobs[k].used)
                print_job(k, jobs[k].stopped ? "stopped " : "running ");
        return 1;
    }
    if (strcmp(name, "fg") == 0 || strcmp(name, "bg") == 0) {
        /*
         * fg：作业到前台：终端的前台换成它的组，发 SIGCONT 让停着的接着运行，再像前台命令一样等它。
         * bg：发 SIGCONT，让它在后台接着运行，不等。
         */
        if (job_control)
            reap_jobs();
        int k = job_control ? find_job(argc, argv) : -1;
        if (k < 0) {
            if (!job_control)
                eprint("sh: no job control\n");
            *status = 1;
            return 1;
        }
        struct job *j = &jobs[k];
        if (name[0] == 'b') {
            j->stopped = 0;
            kill(-j->pgid, SIGCONT);
            print("[");
            print_num((uint32_t)k + 1);
            print("] ");
            print(j->cmd);
            print(" &\n");
            return 1;
        }
        print(j->cmd);
        print("\n");
        set_foreground(j->pgid);
        j->stopped = 0;
        kill(-j->pgid, SIGCONT);
        int32_t pids[MAX_ARGS];
        int npids = j->npids;
        for (int i = 0; i < npids; i++)
            pids[i] = j->pids[i];
        *status = wait_job(j->pgid, pids, 0, npids, 0, k);
        return 1;
    }
    if (strcmp(name, "help") == 0) {
        print("files: ls cat echo wc grep sort rm ln mv mkdir rmdir chmod touch mkfifo pwd\n");
        print("disks: df sync mount umount\n");
        print("tasks: ps kill sleep nice date letter hello check\n");
        print("users: id su login passwd chown\n");
        print("built in: cd exit time test [ true false jobs fg bg help; if while for\n");
        print("try: ls /bin | wc, echo hi > /tmp/a, x=1; echo $((x+1)), letter D &\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ 读命令 */

#define LINE_TOO_LONG (-2)

/*
 * 从 fd 读一行，去掉换行放进 line，返回长度；读完了返回 -1；被信号打断返回 -EINTR，已经读到的半行丢掉；
 * 一行超过 127 个字节返回 LINE_TOO_LONG，整行都不要：截短了照样执行，可能删错文件。
 * 文件一次读进来好几行，多出来的留在 buf 里给下一次。文件的最后一行没有换行也算一行，下一次再返回 -1。
 * 标准输入是管道时也用它：在终端上用的是下面的 edit_line()。
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
    char out[16] = "\x1b[";
    uint32_t n = 2, digits = 1;
    for (uint32_t v = k; v >= 10; v /= 10)
        digits++;
    for (uint32_t i = digits; i > 0; i--, k /= 10)
        out[n + i - 1] = (char)('0' + k % 10);
    n += digits;
    out[n++] = 'D';
    put(out, n);
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

/* ------------------------------------------------------------------ 主循环 */

int main(int sh_argc, char **sh_argv)
{
    char line[LINE_LEN];
    static char text[TEXT_MAX];     /* 正在攒的一条命令：没写完的 if、while、引号会跨好几行 */
    int32_t in = 0;
    int interactive = sh_argc < 2;
    struct stat script;

    shell_pid = getpid();

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
    /* $0 是 sh 或者脚本的名字，$1 起是脚本的参数 */
    pos_args = interactive ? sh_argv : sh_argv + 1;
    pos_count = interactive ? 0 : sh_argc - 2;
    /* 先问一下 SIGINT 原来怎么处理：signal 返回原来的处理方式，问的时候设成忽略，下面马上改回来 */
    sighandler_t old_intr = signal(SIGINT, SIG_IGN);
    intr_ignored = old_intr == SIG_IGN;
    signal(SIGINT, interactive && !intr_ignored ? on_intr : old_intr);  /* Ctrl+C 结束的是前台程序，不是 shell 自己 */
    /* 标准输入是终端（TCGETS 成功）才用行编辑；管道、文件照旧一行行读。终端开机时是规范模式、回显，记下来 */
    int editing = interactive && ioctl(0, TCGETS, &sane) == 0;
    sane.c_lflag |= ICANON | ECHO;
    job_control = editing;
    if (job_control) {
        /* 作业控制：shell 自己不能被 Ctrl+Z 停下，也不该因为读写终端停下；终端的前台先是自己 */
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        shell_pgid = getpgrp();
        set_foreground(shell_pgid);
    }
    if (interactive) {
        signal(SIGTERM, SIG_IGN);   /* kill 0 发给这一组，也不该把 shell 自己结束；kill -9 才行 */
        print("shell on hda: try help, ls /bin, cat /hello.txt, ps\n");
    }
    uint32_t len = 0;               /* text 里已经攒了多少 */
    for (;;) {
        reap_jobs();
        if (interactive)
            print(len ? "> " : "$ ");     /* 没写完的命令：接着读的这一行打 > */
        int32_t n = editing ? edit_line(line) : read_line(in, line);
        if (n == -1) {              /* 读完了：脚本读到末尾，或者在空行上按了 Ctrl+D，像 exit 一样退出 */
            if (len) {
                eprint("sh: syntax error: unexpected end of file\n");
                if (!interactive)
                    return 2;
                len = 0;            /* 交互时只丢掉没写完的这条命令 */
                continue;
            }
            return last_status;
        }
        if (n == LINE_TOO_LONG) {
            eprint("sh: line too long\n");
            continue;
        }
        if (n < 0) {                /* 在提示符上按了 Ctrl+C（-EINTR）：已经回显 ^C，没写完的命令也不要了 */
            len = 0;
            continue;
        }
        if (len + (uint32_t)n + 2 > sizeof(text)) {
            eprint("sh: command too long\n");
            len = 0;
            continue;
        }
        for (int32_t i = 0; i < n; i++)
            text[len++] = line[i];
        text[len++] = '\n';
        text[len] = '\0';

        int ntok = tokenize(text);
        if (ntok == INCOMPLETE)     /* 引号还没配对：再读一行 */
            continue;
        arena_used = 0;
        tpos = 0;
        parse_state = ntok == TOO_LONG ? 1 : 0;
        if (ntok == TOO_LONG)
            eprint("sh: command too long\n");
        struct node *tree = parse_state ? 0 : parse_list();
        if (!parse_state && peek() != T_END)
            syntax_error();         /* 多出来的 fi、done 之类 */
        if (parse_state == INCOMPLETE)
            continue;               /* if 还没有 fi、末尾是 | 之类：再读一行 */
        len = 0;
        if (parse_state) {
            last_status = 2;
            continue;
        }
        interrupted = 0;
        run(tree);
    }
}
