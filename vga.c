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

/* vga_putc() 输出的窗口（含首尾两行）。光标写过最后一行时，窗口内容整体上移一行。 */
static int window_top = 0;
static int window_bottom = VGA_ROWS - 1;

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

/*
 * 设置 vga_putc() 使用的窗口。只有窗口内的行会滚动，窗口外的固定内容
 * （第 0 行的标记、ticks 计数、任务输出等）不受影响。
 */
void vga_set_window(int top, int bottom)
{
    window_top = top;
    window_bottom = bottom;
}

/* 窗口内的行依次上移一行，最后一行清空。VGA_TEXT 是 volatile，编译器不会把循环换成 memmove。 */
static void scroll_window(void)
{
    for (int row = window_top; row < window_bottom; row++)
        for (int col = 0; col < VGA_COLS; col++)
            VGA_TEXT[row * VGA_COLS + col] = VGA_TEXT[(row + 1) * VGA_COLS + col];
    for (int col = 0; col < VGA_COLS; col++)
        put_cell(window_bottom, col, ' ', VGA_GRAY);
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
        /* 退到前一格；在行首时退到上一行行尾，这样自动换行的长输入也能逐字删回去 */
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > window_top) {
            cursor_row--;
            cursor_col = VGA_COLS - 1;
        }
        put_cell(cursor_row, cursor_col, ' ', VGA_GRAY);
    } else {
        put_cell(cursor_row, cursor_col, ch, VGA_GRAY);
        if (++cursor_col == VGA_COLS) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row > window_bottom) {
        scroll_window();
        cursor_row = window_bottom;
    }
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

/*
 * 拼一整行文字，再一次写到屏幕上：异常面板、任务结束的消息等都用它。
 * 不用 = { 0 } 初始化 text，因为编译器可能为此生成对 memset 的调用，而内核里没有 memset。
 */
void line_init(struct text_line *line)
{
    line->len = 0;
    line->text[0] = '\0';
}

void line_append(struct text_line *line, const char *s)
{
    while (*s && line->len < VGA_COLS)
        line->text[line->len++] = *s++;
    line->text[line->len] = '\0';
}

void line_append_dec(struct text_line *line, uint32_t value)
{
    char buf[12];
    line_append(line, format_uint(value, 10, buf));
}

/* 追加 "名字=0x十六进制 "。digits 为 8 或 4（段寄存器只有 16 位）。 */
void line_append_hex(struct text_line *line, const char *name, uint32_t value, int digits)
{
    char buf[12];

    line_append(line, name);
    line_append(line, "=0x");
    line_append(line, format_uint(value, 16, buf) + (8 - digits));
    line_append(line, " ");
}

/* 补空格到 80 列再写到第 row 行，盖住这一行原来的内容；然后清空，可以接着拼下一行。 */
void line_show(struct text_line *line, uint8_t color, int row)
{
    while (line->len < VGA_COLS)
        line_append(line, " ");
    vga_write_at(row, 0, line->text, color);
    line_init(line);
}
