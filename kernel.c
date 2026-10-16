/*
 * 内核的 C 部分：entry.asm 换好栈、清零 .bss 后调用 kmain()。
 *
 * 这里没有标准库，也没有 BIOS，输出只能直接写 VGA 文本显存。
 * 第 0 行已有 ABPK，kmain 接着写 C，再在第 2 行打印一句话。
 */
#include <stdint.h>

#define VGA_TEXT  ((volatile uint16_t *)0xB8000)  /* 每格 2 字节：低字节字符，高字节属性 */
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_GRAY  0x07                             /* 黑底浅灰字 */

/*
 * 光标位置。没有初值的 static 变量放在 .bss，起始值 0 依赖 entry.asm 的清零；
 * 如果没有清零，它们会是磁盘上 kernel.bin 之后的随机内容。
 */
static int cursor_row;
static int cursor_col;

static void vga_putc(char ch)
{
    if (ch == '\n') {
        cursor_row++;
        cursor_col = 0;
        return;
    }
    VGA_TEXT[cursor_row * VGA_COLS + cursor_col] = (uint16_t)(VGA_GRAY << 8 | (uint8_t)ch);
    if (++cursor_col == VGA_COLS) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row == VGA_ROWS)
        cursor_row = 0;         /* 还没有滚屏，写满后回到第 0 行 */
}

static void vga_puts(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

void kmain(void);

void kmain(void)
{
    cursor_col = 4;             /* 第 0 行前 4 格已有 ABPK，从第 0 行第 4 列接着写 */
    vga_putc('C');

    vga_puts("\n\nHello from kmain() in C!");

    for (;;)
        __asm__ volatile("hlt");
}
