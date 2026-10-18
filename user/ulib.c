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
 * malloc 从头找第一个够大的空块（路上把挨着的空块并成一块），大得多就切成两块；找不到就用 sbrk 往上要一块。
 * free 把块标成空的，再和后面紧挨着的空块并成一块。realloc 遇到堆末尾的块，直接 sbrk 让它变大，不用搬。
 * 这是最简单的分配器：真正的 malloc 会按大小分桶，把空块串成链表，还会把堆末尾的空块还给内核。
 */
struct block {
    uint32_t size;                  /* 连块头一共多少字节，8 的倍数 */
    uint32_t free;
};
static struct block *heap_start, *heap_end;

#define NEXT(b) ((struct block *)((char *)(b) + (b)->size))

/* n 个字节的块一共要多大：加上块头，对齐到 8；大得离谱返回 0（比整个用户空间还大，也免得算大小时溢出） */
static uint32_t block_size(uint32_t n)
{
    return n > 0x01000000 ? 0 : (n + sizeof(struct block) + 7) & ~7u;
}

/*
 * 在堆的末尾接一块 size 字节的新块。别人可能直接用 sbrk 往上要过内存：那一段不归 malloc 管，
 * 记成一个占着的块跨过去（顺便补齐到 8 字节），不然扫描时会把那里的 0 当成块头，原地打转。
 */
static struct block *extend(uint32_t size)
{
    uint32_t cur = brk(0), aligned = (cur + 7) & ~7u;
    if (!heap_start) {
        if (sbrk((int32_t)(aligned - cur)) == (void *)-1)
            return 0;
        heap_start = heap_end = (struct block *)aligned;
    } else if (cur != (uint32_t)heap_end) {
        if (sbrk((int32_t)(aligned - cur)) == (void *)-1)
            return 0;
        heap_end->size = aligned - (uint32_t)heap_end;
        heap_end->free = 0;
        heap_end = (struct block *)aligned;
    }
    struct block *b = sbrk((int32_t)size);
    if (b == (void *)-1)
        return 0;
    b->size = size;
    b->free = 0;
    heap_end = NEXT(b);
    return b;
}

void *malloc(uint32_t n)
{
    uint32_t need = block_size(n);
    if (!need)
        return 0;
    for (struct block *b = heap_start; b && b < heap_end; b = NEXT(b)) {
        if (!b->free)
            continue;
        while (NEXT(b) < heap_end && NEXT(b)->free)     /* 先 free 前一块、后 free 后一块的，在这里并上 */
            b->size += NEXT(b)->size;
        if (b->size < need)
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
    struct block *b = extend(need);
    return b ? b + 1 : 0;
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

void *realloc(void *p, uint32_t n)
{
    if (!p)
        return malloc(n);
    struct block *b = (struct block *)p - 1;
    uint32_t need = block_size(n);
    if (!need)
        return 0;
    if (b->size >= need)
        return p;
    if (NEXT(b) == heap_end && brk(0) == (uint32_t)heap_end) {    /* 堆末尾的块：往上推 brk 就行 */
        if (sbrk((int32_t)(need - b->size)) == (void *)-1)
            return 0;
        b->size = need;
        heap_end = NEXT(b);
        return p;
    }
    char *q = malloc(n);
    if (!q)
        return 0;
    for (uint32_t i = 0; i < b->size - sizeof(struct block); i++)
        q[i] = ((char *)p)[i];
    free(p);
    return q;
}
