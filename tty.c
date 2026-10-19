/*
 * 终端：键盘输入和控制台窗口（第 8～18 行），与 Linux 0.11 的 kernel/chr_drv/tty_io.c 对应。
 *
 * 键盘中断把一个个字符交给 tty_input()，这里按“规范模式”处理：
 *   - 普通字符放进正在编辑的这一行，同时回显在屏幕上；
 *   - 退格删掉这一行的最后一个字符，屏幕上也擦掉；这一行删空了就不能再删，删不到程序输出的东西；
 *   - 回车把这一行连同换行符放进 ready 缓冲区，叫醒睡在 tty_wait 上等着读的任务。
 *   - Ctrl+D 把这一行（不带换行）交出去，后面跟一个结束标记；行是空的，读的程序就得到 0，知道输入结束了。
 *     Unix 的终端都是这样，cat、wc 读到 0 就停。
 *   - Ctrl+C 丢掉正在编辑的这一行，回显 ^C 和换行，给终端的前台进程组发 SIGINT（tty_intr）。
 * 关掉回显（termios 的 ECHO，输密码时）以后，敲的字照样进这一行，只是不显示，退格和 ^C 也不显示。
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

static char line[LINE_MAX];
static uint32_t line_len;
static char ready[READY_SIZE];
static uint32_t ready_head, ready_tail;     /* 只增不减，相减就是还没读走的字节数 */
static struct wait_queue tty_wait;
static int tty_pgrp;                /* 前台进程组：Ctrl+C 发给这一组；0 表示还没有（自检内核不拉起 shell） */
static uint32_t tty_lflag = ICANON | ECHO;  /* termios 的 c_lflag：开机时一行一行地读、回显 */

/* 回显一个字符：关了回显就不显示。 */
static void echo(char ch)
{
    if (tty_lflag & ECHO)
        vga_putc(ch);
}

/*
 * 进程组的头打开控制台时调用：这一组就成了前台进程组。init 拉起 shell 前先 setsid()，
 * 所以前台进程组是 shell 这一组，shell fork 出的程序都在里面，init 和它的 letter 不在。
 * Linux 0.11 的 sys_open() 也是在 current->leader 打开终端、它还没有控制终端时，把 tty->pgrp 设成它的组。
 */
void tty_set_pgrp(int pgrp)
{
    tty_pgrp = pgrp;
}

/* 进程组的头结束了：前台进程组还是它那一组的话，清掉。init 拉起的新 shell 打开终端时再设上。 */
void tty_release_pgrp(int pgrp)
{
    if (tty_pgrp == pgrp)
        tty_pgrp = 0;
}

/* 由键盘中断调用（自检也会直接调用），此时中断关着。 */
void tty_input(char ch)
{
    if (ch == INTR_CHAR) {
        /*
         * Linux 0.11 的 copy_to_cooked() 遇到 INTR_CHAR 调用 tty_intr()，给 tty->pgrp 里的每个进程挂上 SIGINT，
         * 不回显，也不丢掉已经敲的字符；这里像后来的 Unix 一样回显 ^C，丢掉这一行。
         */
        line_len = 0;
        echo('^');
        echo('C');
        echo('\n');
        if (tty_pgrp > 0)
            sched_kill(-tty_pgrp, SIGINT);
        return;
    }
    if (ch == '\b') {
        if (line_len > 0) {
            line_len--;
            echo('\b');
        }
        return;
    }
    if (ch == '\n' || ch == EOF_CHAR) {
        if (ch == '\n')
            echo('\n');
        if (READY_SIZE - (ready_tail - ready_head) >= line_len + 1) {   /* 放不下就丢掉这一行 */
            for (uint32_t i = 0; i < line_len; i++)
                ready[ready_tail++ % READY_SIZE] = line[i];
            ready[ready_tail++ % READY_SIZE] = ch;
        }
        line_len = 0;
        wake_up(&tty_wait);
        return;
    }
    if (ch < ' ')                   /* 其余控制字符不认识，丢掉 */
        return;
    if (line_len < LINE_MAX - 1) {
        line[line_len++] = ch;
        echo(ch);
    }
}

/*
 * ioctl：TCGETS 取工作方式，TCSETS 设。现在只认 ECHO；ICANON 还不能关（读的时候总是一行一行的），想关就返回 -1。
 * Linux 0.11 的 tty_ioctl() 把整个 struct termios 存在 tty->termios 里，copy_to_cooked() 按里面的标志处理每个字符。
 */
int tty_ioctl(uint32_t cmd, struct termios *t)
{
    if (cmd == TCGETS) {
        *t = (struct termios){ .c_lflag = tty_lflag };
        return 0;
    }
    if (cmd != TCSETS || !(t->c_lflag & ICANON))
        return -1;
    tty_lflag = t->c_lflag & (ICANON | ECHO);
    return 0;
}

/*
 * read(0)：读走一行，最多 n 个字节，返回读到的字节数。还没有敲完的行就睡下等。
 * 先在关中断时搬进内核里的 tmp，开中断后再复制给用户：复制时可能缺页或写时复制。
 */
int32_t tty_read(void *dst, uint32_t n)
{
    char tmp[LINE_MAX];
    uint32_t got = 0;
    uint32_t flags = irq_save();

    while (ready_tail == ready_head) {
        if (interruptible_sleep_on(&tty_wait) < 0) {
            irq_restore(flags);
            return -EINTR;          /* 等着的时候来了信号，比如按了 Ctrl+C */
        }
    }
    while (got < n && got < LINE_MAX && ready_tail != ready_head) {
        char c = ready[ready_head % READY_SIZE];
        if (c == EOF_CHAR) {        /* 结束标记：读掉它，不交给程序；它前面没有字符时 read 返回 0 */
            ready_head++;
            break;
        }
        ready_head++;
        tmp[got++] = c;
        if (c == '\n')
            break;
    }
    irq_restore(flags);
    for (uint32_t i = 0; i < got; i++)
        ((char *)dst)[i] = tmp[i];
    return (int32_t)got;
}

/* write(1)：写到控制台窗口。每个字符关一下中断，免得和键盘中断里的回显同时移动光标。 */
int32_t tty_write(const void *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t flags = irq_save();
        vga_putc(((const char *)src)[i]);
        irq_restore(flags);
    }
    return (int32_t)n;
}
