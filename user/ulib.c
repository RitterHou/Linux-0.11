#include "ulib.h"

/*
 * 程序入口。exec 把栈摆成：假的返回地址、argc、argv，所以 _start 就是一个普通的 C 函数，
 * 直接拿到 argc 和 argv。main 返回后用它的返回值退出。链接脚本把 .text.start 放在最前面。
 */
__attribute__((section(".text.start"), noreturn)) void _start(int argc, char **argv)
{
    exit(main(argc, argv));
}

uint32_t strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

void print(const char *s)
{
    write(1, s, strlen(s));
}

void print_num(uint32_t n)
{
    char buf[12];
    int i = sizeof(buf);
    do
        buf[--i] = (char)('0' + n % 10);
    while ((n /= 10) != 0);
    write(1, buf + i, sizeof(buf) - (uint32_t)i);
}
