/*
 * 端口 I/O：x86 用 in/out 指令访问 8259、8253、键盘控制器、VGA 等外设的寄存器。
 * 这些端口不在内存地址空间里，C 语言无法直接读写，只能用内联汇编。
 */
#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 向空闲端口 0x80 写一个字节，给老式外设留出处理上一条命令的时间。 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
