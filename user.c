/*
 * 用户程序：任务 B 在特权级 3（用户态）运行这里的代码。
 *
 * 链接脚本把本文件的代码、只读数据和数据单独放进按 4 KB 对齐的 .user 段，
 * paging.c 只给这几页打开“用户”位，其中代码所在的页只读。用户栈在 0x40000000 起的 4 MB 里，
 * 用到哪一页，内核才在缺页异常里分配哪一页（见 paging.c 的 handle_page_fault()）。内核的代码、数据和显存对这里的代码都不可访问，
 * cli、hlt、in、out 等特权指令也不能执行，要做任何事都只能用 int 0x80 请内核帮忙。
 * 所以这里只包含 syscall.h，不包含内核的 kernel.h。
 */
#include "selftest.h"
#include "syscall.h"

#if defined(SELFTEST) && SELFTEST == SELFTEST_USER_WRITE_KERNEL
extern volatile uint32_t timer_ticks;   /* 内核的变量，所在的页没有用户位 */
#endif

/* 与内核任务 A 的 task_main() 做同样的事：任务 2 写 B。 */
void user_main(void)
{
#if !defined(SELFTEST) || !SELFTEST_IN_USER(SELFTEST)
    /* 正常构建：没有用户态自检 */
#elif SELFTEST == SELFTEST_USER_WRITE_KERNEL
    timer_ticks = 0;
#elif SELFTEST == SELFTEST_USER_PRIVILEGED
    __asm__ volatile("cli");
#elif SELFTEST == SELFTEST_USER_BAD_POINTER
    /* 把内核代码的地址交给 write：内核必须拒绝，否则用户程序就能借 write 读出内核内存 */
    int32_t ret = write(1, (const void *)0x10000, 1);
    write(1, ret == -1 ? "K" : "X", 1);
#elif SELFTEST == SELFTEST_USER_EXIT
    write(1, "bye", 3);
    exit(7);
#elif SELFTEST == SELFTEST_USER_WRITE_CODE
    *(volatile uint8_t *)user_main = 0xC3;  /* 改写自己的代码：代码页只读，期望因 #PF 被结束 */
#elif SELFTEST == SELFTEST_USER_SPIN
    /* 做一次系统调用（让内核写出 U），然后一直在用户态空转，便于测试在用户态注入 NMI */
    write(1, "s", 1);
    for (;;)
        __asm__ volatile("");
#elif SELFTEST == SELFTEST_USER_DEEP_STACK
    /*
     * 在栈上用掉 5 页：碰到的每一页第一次都缺页，内核补上一页。最低的那一页自己不碰，
     * 直接交给 write，由内核去读：内核态碰到栈区里没映射的页也要补上，write 返回 1，再写一个 k。
     * 最后 exit(3)，内核收回所有栈页和页表。
     */
    volatile char deep[5 * 4096];
    for (int i = 1; i < 5; i++)
        deep[i * 4096] = 'd';
    int32_t ret = write(1, (const void *)deep, 1);
    write(1, ret == 1 ? "k" : "x", 1);
    exit(3);
#elif SELFTEST == SELFTEST_USER_WILD_READ
    (void)*(volatile uint32_t *)0x50000000;     /* 不在栈区：内核不能补页，只能结束任务 B */
#elif SELFTEST == SELFTEST_ALL_EXIT
    write(1, "bye", 3);
    exit(2);
#else
#error "未知的用户态 SELFTEST"
#endif

    char letter = (char)('A' + getpid() - 1);

    for (;;) {
        write(1, &letter, 1);
        sleep(2);
    }
}
