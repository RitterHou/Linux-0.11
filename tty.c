/*
 * 终端，与 Linux 0.11 的 kernel/chr_drv/tty_io.c 对应。有两个：
 *   0：控制台：键盘输入，输出写在第 8～18 行的窗口里（/dev/tty0）；
 *   1：串口：COM1 另一头的终端，收到的字符由串口中断送来，输出从串口发出去（/dev/ttyS0，见 serial.c）。
 * 每个终端有自己的一行、读队列、等着读的任务、前台进程组和工作方式，互不相干：控制台上的 Ctrl+C 不会结束串口上的程序。
 * Linux 0.11 的 tty_table[3] 也是控制台加两个串口，每个 tty 有自己的队列和 termios，写的时候调用各自的 write 函数。
 *
 * 键盘中断（或者串口中断）把一个个字符交给 tty_input()，这里按“规范模式”处理：
 *   - 普通字符放进正在编辑的这一行，同时回显在屏幕上；
 *   - 退格删掉这一行的最后一个字符，屏幕上也擦掉；这一行删空了就不能再删，删不到程序输出的东西；
 *   - 回车把这一行连同换行符放进 ready 缓冲区，叫醒睡在 tty_wait 上等着读的任务。
 *   - Ctrl+D 把这一行（不带换行）交出去，后面跟一个结束标记；行是空的，读的程序就得到 0，知道输入结束了。
 *     Unix 的终端都是这样，cat、wc 读到 0 就停。
 *   - Ctrl+C 丢掉正在编辑的这一行，回显 ^C 和换行，给终端的前台进程组发 SIGINT（tty_intr）。
 * 关掉回显（termios 的 ECHO，输密码时）以后，敲的字照样进这一行，只是不显示，退格和 ^C 也不显示。
 * 串口那头的终端敲回车送来的是 \r，退格送来的是 DEL（0x7F）：分别当成换行和退格（termios 的 ICRNL 和 VERASE）。
 * read(0) 一次最多读走一行；还没有敲完的行，程序读不到，它就睡下等。
 * 程序 write(1) 的输出也写在这个窗口里，和回显共用一个光标，就像一台真正的终端。
 * Linux 0.11 的终端有三个队列：read_q 存键盘送来的原始字符，copy_to_cooked() 处理退格和回显，
 * 放进 secondary 队列，tty_read() 从 secondary 里读。这里把前两步合在键盘中断里做。
 */
#include "io.h"
#include "kernel.h"
#include "syscall.h"

#define LINE_MAX   128              /* 一行最多 127 个字符，再加换行；比屏幕宽，长的一行会折到下一行 */
#define READY_SIZE 512              /* 敲完、还没被读走的行；2 的幂，下标回绕时取模也对 */
#define EOF_CHAR   0x04             /* Ctrl+D；在 ready 里当作一行的结束标记，不交给程序 */
#define INTR_CHAR  0x03             /* Ctrl+C */

struct tty {
    char line[LINE_MAX];            /* 正在编辑的这一行 */
    uint32_t line_len;
    char ready[READY_SIZE];         /* 敲完了、等着读走的 */
    uint32_t ready_head, ready_tail;    /* 只增不减，相减就是还没读走的字节数 */
    struct wait_queue wait;         /* 等着读的任务 */
    int pgrp;                       /* 前台进程组：Ctrl+C 发给这一组；0 表示还没有（自检内核不拉起 shell） */
    uint32_t lflag;                 /* termios 的 c_lflag：开机时一行一行地读、回显 */
    void (*putc)(char ch);          /* 输出一个字符：写显存，或者从串口发出去 */
};

static struct tty ttys[NR_TTYS] = {
    [TTY_CONSOLE] = { .lflag = ICANON | ECHO, .putc = vga_putc },
    [TTY_SERIAL] = { .lflag = ICANON | ECHO, .putc = serial_putc },
};

/* 回显一个字符：关了回显就不显示。 */
static void echo(struct tty *t, char ch)
{
    if (t->lflag & ECHO)
        t->putc(ch);
}

/*
 * 进程组的头打开控制台时调用：这一组就成了前台进程组。init 拉起 shell 前先 setsid()，
 * 所以前台进程组是 shell 这一组，shell fork 出的程序都在里面，init 和它的 letter 不在。
 * Linux 0.11 的 sys_open() 也是在 current->leader 打开终端、它还没有控制终端时，把 tty->pgrp 设成它的组。
 */
void tty_set_pgrp(int tty, int pgrp)
{
    ttys[tty].pgrp = pgrp;
}

/* 进程组的头结束了：前台进程组还是它那一组的话，清掉。init 拉起的新 shell 打开终端时再设上。 */
void tty_release_pgrp(int tty, int pgrp)
{
    if (ttys[tty].pgrp == pgrp)
        ttys[tty].pgrp = 0;
}

/* 由键盘中断或串口中断调用（自检也会直接调用），此时中断关着。 */
void tty_input(int tty, char ch)
{
    struct tty *t = &ttys[tty];

    if (ch == '\r')                 /* 终端的回车键 */
        ch = '\n';
    if (ch == 0x7F)                 /* 终端的退格键 */
        ch = '\b';
    if (ch == INTR_CHAR) {
        /*
         * Linux 0.11 的 copy_to_cooked() 遇到 INTR_CHAR 调用 tty_intr()，给 tty->pgrp 里的每个进程挂上 SIGINT，
         * 不回显，也不丢掉已经敲的字符；这里像后来的 Unix 一样回显 ^C，丢掉这一行。
         */
        t->line_len = 0;
        echo(t, '^');
        echo(t, 'C');
        echo(t, '\n');
        if (t->pgrp > 0)
            sched_kill(-t->pgrp, SIGINT);
        return;
    }
    if (ch == '\b') {
        if (t->line_len > 0) {
            t->line_len--;
            echo(t, '\b');
        }
        return;
    }
    if (ch == '\n' || ch == EOF_CHAR) {
        if (ch == '\n')
            echo(t, '\n');
        if (READY_SIZE - (t->ready_tail - t->ready_head) >= t->line_len + 1) {   /* 放不下就丢掉这一行 */
            for (uint32_t i = 0; i < t->line_len; i++)
                t->ready[t->ready_tail++ % READY_SIZE] = t->line[i];
            t->ready[t->ready_tail++ % READY_SIZE] = ch;
        }
        t->line_len = 0;
        wake_up(&t->wait);
        return;
    }
    if ((uint8_t)ch < ' ')          /* 其余控制字符不认识，丢掉 */
        return;
    if (t->line_len < LINE_MAX - 1) {
        t->line[t->line_len++] = ch;
        echo(t, ch);
    }
}

/*
 * ioctl：TCGETS 取工作方式，TCSETS 设。现在只认 ECHO；ICANON 还不能关（读的时候总是一行一行的），想关就返回 -1。
 * Linux 0.11 的 tty_ioctl() 把整个 struct termios 存在 tty->termios 里，copy_to_cooked() 按里面的标志处理每个字符。
 */
int tty_ioctl(int tty, uint32_t cmd, struct termios *t)
{
    if (cmd == TCGETS) {
        *t = (struct termios){ .c_lflag = ttys[tty].lflag };
        return 0;
    }
    if (cmd != TCSETS || !(t->c_lflag & ICANON))
        return -1;
    ttys[tty].lflag = t->c_lflag & (ICANON | ECHO);
    return 0;
}

/*
 * read(0)：读走一行，最多 n 个字节，返回读到的字节数。还没有敲完的行就睡下等。
 * 先在关中断时搬进内核里的 tmp，开中断后再复制给用户：复制时可能缺页或写时复制。
 */
int32_t tty_read(int tty, void *dst, uint32_t n)
{
    struct tty *t = &ttys[tty];
    char tmp[LINE_MAX];
    uint32_t got = 0;
    uint32_t flags = irq_save();

    while (t->ready_tail == t->ready_head) {
        if (interruptible_sleep_on(&t->wait) < 0) {
            irq_restore(flags);
            return -EINTR;          /* 等着的时候来了信号，比如按了 Ctrl+C */
        }
    }
    while (got < n && got < LINE_MAX && t->ready_tail != t->ready_head) {
        char c = t->ready[t->ready_head % READY_SIZE];
        if (c == EOF_CHAR) {        /* 结束标记：读掉它，不交给程序；它前面没有字符时 read 返回 0 */
            t->ready_head++;
            break;
        }
        t->ready_head++;
        tmp[got++] = c;
        if (c == '\n')
            break;
    }
    irq_restore(flags);
    for (uint32_t i = 0; i < got; i++)
        ((char *)dst)[i] = tmp[i];
    return (int32_t)got;
}

/* write(1)：写到终端。每个字符关一下中断，免得和中断里的回显同时移动光标、同时往串口送。 */
int32_t tty_write(int tty, const void *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t flags = irq_save();
        ttys[tty].putc(((const char *)src)[i]);
        irq_restore(flags);
    }
    return (int32_t)n;
}
