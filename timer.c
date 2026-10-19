/*
 * 8253/8254 可编程定时器：通道 0 接在 8259 的 IRQ0 上。
 * 它把 1.193180 MHz 的输入时钟分频，每数完 latch 个脉冲就产生一次 IRQ0。
 * Linux 0.11 在 sched_init() 中做同样的设置，定时器中断驱动进程调度。
 */
#include "io.h"
#include "kernel.h"
#include "syscall.h"

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_INPUT_HZ  1193180

#define TICKS_ROW 4
#define TICKS_COL 7                 /* 紧跟在 kmain 写的 "ticks: " 后面 */

volatile uint32_t timer_ticks;      /* 开机以来的定时器中断次数 */
static uint32_t startup_time;       /* 开机时的时间：1970 年以来的秒数（UTC） */

#define CLOCK_ROW 4
#define CLOCK_COL 40                /* 第 4 行右边：2026-10-19 05:12:33 UTC */

/* CMOS 里的实时时钟：往 0x70 写寄存器号，从 0x71 读。Linux 0.11 写的是 0x80|寄存器号，顺便屏蔽了 NMI；这里不屏蔽。 */
static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 15);
}

/*
 * 开机时读一次 CMOS 时钟，记下开机时间；以后的“现在”是开机时间加上 timer_ticks / 100 秒。
 * 时钟在更新的那一刻（寄存器 A 的第 7 位）读出来可能对不上，就等它更新完；读完再读一遍秒，变了就重读。
 * 寄存器 B 的第 2 位说明数是二进制还是 BCD（QEMU 默认 BCD），第 1 位说明是 24 小时制还是 12 小时制。
 * 年份只有两位：世纪在寄存器 0x32（QEMU 有），读不出合理的值就当 20xx。
 * Linux 0.11 的 time_init() 也是读到秒数前后一致为止，再用 kernel_mktime() 换算。
 */
void time_init(void)
{
    struct date d;
    uint8_t b = cmos(0x0B), sec, hour, century;

    do {
        while (cmos(0x0A) & 0x80)
            ;
        sec = cmos(0x00);
        d.minute = cmos(0x02);
        hour = cmos(0x04);
        d.day = cmos(0x07);
        d.month = cmos(0x08);
        d.year = cmos(0x09);
        century = cmos(0x32);
    } while (sec != cmos(0x00));
    int pm = !(b & 2) && (hour & 0x80);
    hour &= 0x7F;
    if (!(b & 4)) {                 /* BCD */
        sec = (uint8_t)bcd(sec);
        d.minute = bcd((uint8_t)d.minute);
        hour = (uint8_t)bcd(hour);
        d.day = bcd((uint8_t)d.day);
        d.month = bcd((uint8_t)d.month);
        d.year = bcd((uint8_t)d.year);
        century = (uint8_t)bcd(century);
    }
    d.second = sec;
    d.hour = !(b & 2) ? hour % 12 + (pm ? 12 : 0) : hour;
    d.year += (century >= 19 && century <= 21) ? century * 100 : 2000;
    startup_time = date_to_unix(&d) - timer_ticks / TIMER_HZ;
    clock_show();
}

uint32_t current_time(void)
{
    return startup_time + timer_ticks / TIMER_HZ;
}

/* 在第 4 行右边显示现在的时间（UTC），每秒调用一次。 */
void clock_show(void)
{
    struct date d;
    char buf[12];
    char text[24] = "0000-00-00 00:00:00 UTC";
    int value[6], at[6] = { 0, 5, 8, 11, 14, 17 }, width[6] = { 4, 2, 2, 2, 2, 2 };

    unix_to_date(current_time(), &d);
    value[0] = d.year, value[1] = d.month, value[2] = d.day, value[3] = d.hour, value[4] = d.minute, value[5] = d.second;
    for (int i = 0; i < 6; i++) {
        const char *digits = format_uint((uint32_t)value[i], 10, buf);
        int len = 0;
        while (digits[len])
            len++;
        for (int k = 0; k < len; k++)
            text[at[i] + width[i] - len + k] = digits[k];
    }
    vga_write_at(CLOCK_ROW, CLOCK_COL, text, VGA_GRAY);
}

/* latch 写进 16 位计数器，所以 hz 至少要 19（1193180 / 19 ≈ 62799 < 65536）。 */
void timer_init(uint32_t hz)
{
    uint32_t latch = PIT_INPUT_HZ / hz;

    outb(PIT_COMMAND, 0x36);        /* 通道 0，先写低字节再写高字节，方式 3（方波） */
    outb(PIT_CHANNEL0, latch & 0xFF);
    outb(PIT_CHANNEL0, latch >> 8);
}

void timer_handle(void)
{
    char buf[12];

    timer_ticks++;
    vga_write_at(TICKS_ROW, TICKS_COL, format_uint(timer_ticks, 10, buf), VGA_GRAY);
    schedule();                     /* 叫醒到时间的任务，必要时切换。Linux 0.11 的 do_timer() 在内核态时不切换，这里任何时候都可能切换 */
}
