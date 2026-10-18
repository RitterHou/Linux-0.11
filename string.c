/*
 * 编译器需要的两个函数。即使用了 -ffreestanding -fno-builtin，clang 和 gcc 仍可能把大结构体的
 * 赋值、清零（例如 tasks[n] = (struct task){ ... }）编译成对 memset、memcpy 的调用，所以内核要自己提供。
 * Linux 0.11 在 include/string.h 里用内联汇编写了它们。
 * -fno-builtin 保证下面的循环不会又被编译成对自己的调用。
 */
#include "kernel.h"

void *memset(void *dst, int c, uint32_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}
