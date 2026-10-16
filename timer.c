/*
 * 8253/8254 可编程定时器：通道 0 接在 8259 的 IRQ0 上。
 * 它把 1.193180 MHz 的输入时钟分频，每数完 latch 个脉冲就产生一次 IRQ0。
 * Linux 0.11 在 sched_init() 中做同样的设置，定时器中断驱动进程调度。
 */
#include "io.h"
#include "kernel.h"

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_INPUT_HZ  1193180

#define TICKS_ROW 4
#define TICKS_COL 7                 /* 紧跟在 kmain 写的 "ticks: " 后面 */

volatile uint32_t timer_ticks;      /* 开机以来的定时器中断次数 */

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
    schedule();                     /* 时间片用完时切换任务。Linux 0.11 的 do_timer() 在内核态时不切换，这里任何时候都可能切换 */
}
