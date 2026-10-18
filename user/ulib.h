/*
 * 用户程序的小函数库：系统调用的包装（syscall.h）、程序入口，以及几个字符串和输出函数。
 * 用户程序没有 C 标准库，这里只有它们真正用到的几样。
 */
#ifndef ULIB_H
#define ULIB_H

#include "../syscall.h"

int main(int argc, char **argv);

uint32_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void print(const char *s);          /* 写到 1 号描述符 */
void print_num(uint32_t n);
void *sbrk(int32_t increment);      /* 把堆往上推 increment 个字节，返回原来的末尾；失败返回 (void *)-1 */
void *malloc(uint32_t n);           /* 从堆上要 n 个字节，要不到返回 0 */
void free(void *p);
void *realloc(void *p, uint32_t n); /* 把 p 那一块改成 n 个字节，内容照留；在堆末尾就原地变大 */
/* 可以和 malloc 混着用 sbrk 往上要内存，但不要把 brk 降到 malloc 用过的地方以下 */

#endif
