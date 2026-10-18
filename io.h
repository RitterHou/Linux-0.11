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

/* 从端口连续读 count 个 16 位的字到 buf，硬盘的数据端口就这样读。 */
static inline void insw(uint16_t port, void *buf, uint32_t count)
{
    __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
    __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

/*
 * 关中断并返回原来的 EFLAGS；irq_restore() 再恢复原状。
 * 比直接 cli/sti 更安全：调用者原本就关着中断时，结束后不会被意外打开。
 */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

/* 向空闲端口 0x80 写一个字节，给老式外设留出处理上一条命令的时间。 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
