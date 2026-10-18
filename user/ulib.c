#include "ulib.h"

/*
 * 程序入口。exec 把栈摆成：假的返回地址、argc、argv，所以 _start 就是一个普通的 C 函数，
 * 直接拿到 argc 和 argv。main 返回后用它的返回值退出。链接脚本把 .text.start 放在最前面。
 */
__attribute__((section(".text.start"), noreturn)) void _start(int argc, char **argv)
{
    exit(main(argc, argv));
}

/*
 * 信号处理函数返回到这里：内核把它的地址放在处理函数的返回地址上（见 interrupt.c 的 handle_signals()）。
 * 此时栈上依次是信号编号、eax、ecx、edx、EFLAGS、被打断的地方：跳过信号编号，弹回这几个寄存器，
 * 再 ret 回去。Linux 0.11 的 libc 里的 __sig_restore 一模一样。
 */
__asm__(".globl sig_restore\n"
        "sig_restore:\n"
        "    addl $4, %esp\n"
        "    popl %eax\n"
        "    popl %ecx\n"
        "    popl %edx\n"
        "    popfl\n"
        "    ret\n");

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

/* 堆：brk 往上推就行，用到的页缺页时内核才补上。 */
void *sbrk(int32_t increment)
{
    uint32_t old = brk(0);
    if (increment != 0 && brk(old + (uint32_t)increment) != old + (uint32_t)increment)
        return (void *)-1;
    return (void *)old;
}

/*
 * malloc 和 free：堆上的块一个挨一个，每块前面 8 个字节记着这一块有多大（连这 8 个字节）、空着没有。
 * malloc 从头找第一个够大的空块，大得多就切成两块；找不到就用 sbrk 往上要一块。
 * free 把块标成空的，再和后面紧挨着的空块并成一块。这是最简单的分配器：真正的 malloc 会按大小分桶，
 * 把空块串成链表，还会把堆末尾的空块还给内核。
 */
struct block {
    uint32_t size;                  /* 连块头一共多少字节，8 的倍数 */
    uint32_t free;
};
static struct block *heap_start, *heap_end;

#define NEXT(b) ((struct block *)((char *)(b) + (b)->size))

void *malloc(uint32_t n)
{
    if (n > 0x01000000)             /* 比整个用户空间还大：要不到，也免得下面算大小时溢出 */
        return 0;
    uint32_t need = (n + sizeof(struct block) + 7) & ~7u;
    if (!heap_start) {
        uint32_t start = brk(0);
        if (sbrk((int32_t)((8 - (start & 7)) & 7)) == (void *)-1)   /* 块都对齐到 8 字节 */
            return 0;
        heap_start = heap_end = (struct block *)brk(0);
    }
    for (struct block *b = heap_start; b < heap_end; b = NEXT(b)) {
        if (!b->free || b->size < need)
            continue;
        if (b->size >= need + sizeof(struct block) + 8) {   /* 剩下的还够一块：切开 */
            struct block *rest = (struct block *)((char *)b + need);
            rest->size = b->size - need;
            rest->free = 1;
            b->size = need;
        }
        b->free = 0;
        return b + 1;
    }
    struct block *b = sbrk((int32_t)need);
    if (b == (void *)-1)
        return 0;
    b->size = need;
    b->free = 0;
    heap_end = NEXT(b);
    return b + 1;
}

void free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    b->free = 1;
    while (NEXT(b) < heap_end && NEXT(b)->free)
        b->size += NEXT(b)->size;
}
