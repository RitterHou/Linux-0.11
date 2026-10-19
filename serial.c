/*
 * 串口：PC 的 COM1，一片 8250/16550 UART，I/O 端口 0x3F8～0x3FF，接在 8259 的 IRQ4 上。
 * 另一头接一台终端（QEMU 里是一个 socket 或者宿主机的终端窗口），用户在那上面登录：/dev/ttyS0 就是它。
 * 与 Linux 0.11 的 kernel/chr_drv/serial.c、rs_io.s 对应：它在 rs_init() 里设好波特率、打开中断，
 * 收到字符时中断处理把它放进 tty 的读队列；发送也用中断，一次送一个字符。这里发送直接等 UART 空闲再写（轮询），
 * 简单得多，代价是写的时候 CPU 在原地等：QEMU 里的 UART 马上就空，真机上 9600 波特一个字符要等 1 ms。
 *
 * 几个寄存器（端口 0x3F8 加偏移）：
 *   +0  收到的字符 / 要发的字符；DLAB=1 时是波特率除数的低字节
 *   +1  中断允许：位 0 收到字符时中断；DLAB=1 时是除数的高字节
 *   +3  线路控制：位 7 是 DLAB，低两位 11 是 8 个数据位，没有校验，1 个停止位（8N1）
 *   +4  调制解调器控制：DTR、RTS，还有 OUT2：PC 上 OUT2 不开，UART 的中断送不到 8259
 *   +5  线路状态：位 0 有收到的字符，位 5 发送寄存器空了
 *   +7  暂存寄存器：随便写读，开机时用它看看有没有这个 UART
 */
#include "io.h"
#include "kernel.h"

#define COM1        0x3F8
#define UART_DATA   (COM1 + 0)
#define UART_IER    (COM1 + 1)
#define UART_LCR    (COM1 + 3)
#define UART_MCR    (COM1 + 4)
#define UART_LSR    (COM1 + 5)
#define UART_SCRATCH (COM1 + 7)
#define BAUD_DIVISOR 12             /* 115200 / 12 = 9600 波特。QEMU 不管波特率，真的终端两头要设得一样 */

static int present;                 /* 有这个 UART：QEMU 用 -serial none 启动时没有 */

void serial_init(void)
{
    outb(UART_SCRATCH, 0x5A);       /* 没有这片芯片时读出来是 0xFF */
    if (inb(UART_SCRATCH) != 0x5A)
        return;
    present = 1;
    outb(UART_IER, 0);              /* 设置的时候先别中断 */
    outb(UART_LCR, 0x80);           /* DLAB=1：接下来两个端口是波特率除数 */
    outb(UART_DATA, BAUD_DIVISOR & 0xFF);
    outb(UART_IER, BAUD_DIVISOR >> 8);
    outb(UART_LCR, 0x03);           /* DLAB=0，8N1 */
    outb(UART_MCR, 0x0B);           /* DTR、RTS、OUT2 */
    while (inb(UART_LSR) & 1)       /* 读掉开机前就收到的字符 */
        inb(UART_DATA);
    outb(UART_IER, 0x01);           /* 收到字符时中断 */
    pic_unmask(4);
}

int serial_present(void)
{
    return present;
}

/* IRQ4：把收到的字符都交给串口的 tty。一次中断可能已经到了好几个（16550 有 16 字节的 FIFO）。 */
void serial_handle(void)
{
    while (present && (inb(UART_LSR) & 1))
        tty_input(TTY_SERIAL, (char)inb(UART_DATA));
}

static void put_raw(char ch)
{
    while (!(inb(UART_LSR) & 0x20))
        ;
    outb(UART_DATA, (uint8_t)ch);
}

/*
 * 发一个字符。终端的换行要回车加换行两个字符（\r\n），退格要“退一格、写空格盖掉、再退一格”才能擦掉：
 * 这是 termios 里 OPOST、ONLCR 管的事，Linux 0.11 在 tty_write() 里转换。
 */
void serial_putc(char ch)
{
    if (!present)
        return;
    if (ch == '\n')
        put_raw('\r');
    put_raw(ch);
    if (ch == '\b') {
        put_raw(' ');
        put_raw('\b');
    }
}
