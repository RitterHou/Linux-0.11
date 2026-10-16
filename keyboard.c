/*
 * 键盘：每按下或松开一个键，键盘控制器把扫描码放进 0x60 端口，并发出 IRQ1。
 * 这里把常用键的扫描码（第 1 套，美式布局）翻译成字符并回显，支持 Shift 和 Caps Lock。
 */
#include "io.h"
#include "kernel.h"

#define KBD_DATA     0x60
#define KBD_STATUS   0x64
#define KBD_OUT_FULL 0x01           /* 状态位 0：输出缓冲区里有一个字节等着读 */
#define KEY_RELEASED 0x80           /* 扫描码最高位为 1 表示松开按键 */
#define SC_EXTENDED  0xE0           /* 方向键等扩展键先发这个前缀，再发一个字节 */
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_CAPS_LOCK 0x3A

static const char normal_map[0x3A] = {
    [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    [0x10] = 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    [0x1E] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    [0x2B] = '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    [0x39] = ' ',
};

static const char shift_map[0x3A] = {
    [0x02] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    [0x10] = 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    [0x1E] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    [0x2B] = '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    [0x39] = ' ',
};

static int shift_held;              /* 位 0：左 Shift 按住；位 1：右 Shift 按住 */
static int caps_lock;               /* Caps Lock 是否打开（键盘上的指示灯暂不控制） */
static int skip_next;               /* 上一个字节是 0xE0，下一个字节属于扩展键 */
static int line_length;             /* 当前行已输入的字符数，退格不能删到行首之前 */

/* 由扫描码得到字符：Caps Lock 只影响字母，与 Shift 同时生效时字母反而是小写。 */
static char translate(uint8_t key)
{
    char base = normal_map[key];
    int is_letter = base >= 'a' && base <= 'z';
    int upper = is_letter ? (shift_held != 0) != caps_lock : shift_held != 0;
    return upper ? shift_map[key] : base;
}

/*
 * 在 pic_init() 之后、打开中断之前调用：读光键盘控制器里残留的字节。
 * 若开中断前就有按键（开机时按住某个键、BIOS 菜单的松键码），这个字节一直没人读，
 * 键盘控制器就让 IRQ1 保持高电平。8259 是边沿触发，而 ICW1 会重置它的边沿检测，
 * 之后再也等不到上升沿，键盘中断永远不来，也就永远没人读 0x60。读掉之后电平回落，
 * 下一次按键就能产生新的上升沿。
 */
void keyboard_init(void)
{
    while (inb(KBD_STATUS) & KBD_OUT_FULL)
        inb(KBD_DATA);
}

void keyboard_handle(void)
{
    uint8_t scancode = inb(KBD_DATA);   /* 必须读走扫描码，控制器才会送来下一个 */

    if (scancode == SC_EXTENDED) {
        skip_next = 1;
        return;
    }
    if (skip_next) {                    /* 方向键、右 Ctrl 等扩展键，暂不处理 */
        skip_next = 0;
        return;
    }

    int released = scancode & KEY_RELEASED;
    uint8_t key = scancode & ~KEY_RELEASED;

    if (key == SC_LSHIFT || key == SC_RSHIFT) {
        int bit = (key == SC_LSHIFT) ? 1 : 2;
        shift_held = released ? (shift_held & ~bit) : (shift_held | bit);
        return;
    }
    if (released)
        return;
    if (key == SC_CAPS_LOCK) {
        caps_lock = !caps_lock;
        return;
    }
    if (key >= sizeof(normal_map) || normal_map[key] == 0)
        return;

    char ch = translate(key);
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
