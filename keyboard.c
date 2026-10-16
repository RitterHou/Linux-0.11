/*
 * 键盘：每按下或松开一个键，键盘控制器把扫描码放进 0x60 端口，并发出 IRQ1。
 * 这里只处理按下，把常用键的扫描码（第 1 套）翻译成字符并回显，暂不支持 Shift 等组合键。
 */
#include "io.h"
#include "kernel.h"

#define KBD_DATA     0x60
#define KEY_RELEASED 0x80           /* 扫描码最高位为 1 表示松开按键 */

static const char scancode_to_ascii[0x3A] = {
    [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    [0x10] = 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    [0x1E] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    [0x2B] = '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    [0x39] = ' ',
};

static int line_length;             /* 当前行已输入的字符数，退格不能删到行首之前 */

void keyboard_handle(void)
{
    uint8_t scancode = inb(KBD_DATA);   /* 必须读走扫描码，控制器才会送来下一个 */

    if (scancode & KEY_RELEASED || scancode >= sizeof(scancode_to_ascii))
        return;

    char ch = scancode_to_ascii[scancode];
    if (ch == 0)
        return;

    if (ch == '\b') {
        if (line_length > 0) {
            vga_putc('\b');
            line_length--;
        }
        return;
    }

    vga_putc(ch);
    line_length = (ch == '\n') ? 0 : line_length + 1;
}
