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

#endif
