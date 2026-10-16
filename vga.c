/*
 * VGA 文本模式输出：80 × 25 个字符格，从物理地址 0xB8000 开始，
 * 每格 2 字节：低字节字符，高字节属性。没有 BIOS 可用，只能直接写显存。
 */
#include "io.h"
#include "kernel.h"

#define VGA_TEXT  ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

#define VGA_CRTC_INDEX 0x3D4        /* 显卡 CRT 控制器：先写寄存器编号，再写数据 */
#define VGA_CRTC_DATA  0x3D5

/*
 * 光标位置。没有初值的 static 变量放在 .bss，起始值 0 依赖 entry.asm 的清零；
 * 如果没有清零，它们会是磁盘上 kernel.bin 之后的随机内容。
 */
static int cursor_row;
static int cursor_col;

static void put_cell(int row, int col, char ch, uint8_t color)
{
    VGA_TEXT[row * VGA_COLS + col] = (uint16_t)(color << 8 | (uint8_t)ch);
}

/* 让屏幕上闪烁的硬件光标跟着软件光标走（CRTC 寄存器 0x0E、0x0F 是光标位置）。 */
static void update_hardware_cursor(void)
{
    uint16_t pos = (uint16_t)(cursor_row * VGA_COLS + cursor_col);

    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, pos & 0xFF);
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, pos >> 8);
}

void vga_set_cursor(int row, int col)
{
    cursor_row = row;
    cursor_col = col;
    update_hardware_cursor();
}

void vga_putc(char ch)
{
    if (ch == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else if (ch == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            put_cell(cursor_row, cursor_col, ' ', VGA_GRAY);
        }
    } else {
        put_cell(cursor_row, cursor_col, ch, VGA_GRAY);
        if (++cursor_col == VGA_COLS) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row == VGA_ROWS)
        cursor_row = 0;             /* 还没有滚屏，写满后回到第 0 行 */
    update_hardware_cursor();
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

/* 在指定位置写字符串，不移动光标。用于定时器计数、异常信息等固定位置的输出。 */
void vga_write_at(int row, int col, const char *s, uint8_t color)
{
    while (*s && col < VGA_COLS)
        put_cell(row, col++, *s++, color);
}

/*
 * 把无符号整数转成字符串，写进 buf 并返回 buf。
 * 十进制不补零；十六进制固定 8 位，前面补 0，方便对齐地址。
 */
char *format_uint(uint32_t value, unsigned base, char buf[12])
{
    char tmp[12];
    int len = 0;
    int min_digits = (base == 16) ? 8 : 1;

    do {
        unsigned digit = value % base;
        tmp[len++] = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        value /= base;
    } while (value != 0 || len < min_digits);

    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return buf;
}
