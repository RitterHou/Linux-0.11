/*
 * 终端：键盘输入和控制台窗口（第 8～18 行），与 Linux 0.11 的 kernel/chr_drv/tty_io.c 对应。
 *
 * 键盘中断把一个个字符交给 tty_input()，这里按“规范模式”处理：
 *   - 普通字符放进正在编辑的这一行，同时回显在屏幕上；
 *   - 退格删掉这一行的最后一个字符，屏幕上也擦掉；这一行删空了就不能再删，删不到程序输出的东西；
 *   - 回车把这一行连同换行符放进 ready 缓冲区，叫醒睡在 tty_wait 上等着读的任务。
 * read(0) 一次最多读走一行；还没有敲完的行，程序读不到，它就睡下等。
 * 程序 write(1) 的输出也写在这个窗口里，和回显共用一个光标，就像一台真正的终端。
 * Linux 0.11 的终端有三个队列：read_q 存键盘送来的原始字符，copy_to_cooked() 处理退格和回显，
 * 放进 secondary 队列，tty_read() 从 secondary 里读。这里把前两步合在键盘中断里做。
 */
#include "io.h"
#include "kernel.h"

#define LINE_MAX   128              /* 一行最多 127 个字符，再加换行；比屏幕宽，长的一行会折到下一行 */
#define READY_SIZE 512              /* 敲完、还没被读走的行；2 的幂，下标回绕时取模也对 */

static char line[LINE_MAX];
static uint32_t line_len;
static char ready[READY_SIZE];
static uint32_t ready_head, ready_tail;     /* 只增不减，相减就是还没读走的字节数 */
static struct wait_queue tty_wait;

/* 由键盘中断调用（自检也会直接调用），此时中断关着。 */
void tty_input(char ch)
{
    if (ch == '\b') {
        if (line_len > 0) {
            line_len--;
            vga_putc('\b');
        }
        return;
    }
    if (ch == '\n') {
        vga_putc('\n');
        if (READY_SIZE - (ready_tail - ready_head) >= line_len + 1) {   /* 放不下就丢掉这一行 */
            for (uint32_t i = 0; i < line_len; i++)
                ready[ready_tail++ % READY_SIZE] = line[i];
            ready[ready_tail++ % READY_SIZE] = '\n';
        }
        line_len = 0;
        wake_up(&tty_wait);
        return;
    }
    if (line_len < LINE_MAX - 1) {
        line[line_len++] = ch;
        vga_putc(ch);
    }
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

    while (ready_tail == ready_head)
        sleep_on(&tty_wait);
    while (got < n && got < LINE_MAX && ready_tail != ready_head) {
        char c = ready[ready_head++ % READY_SIZE];
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
