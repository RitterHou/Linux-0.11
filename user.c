/*
 * 用户程序：任务 B 在特权级 3（用户态）运行这里的代码。
 *
 * 链接脚本把本文件的代码、只读数据、数据和用户栈单独放进按 4 KB 对齐的 .user 段，
 * paging.c 只给这几页打开“用户”位。内核的代码、数据和显存对这里的代码都不可访问，
 * cli、hlt、in、out 等特权指令也不能执行，要做任何事都只能用 int 0x80 请内核帮忙。
 * 所以这里只包含 syscall.h，不包含内核的 kernel.h。
 */
#include "selftest.h"
#include "syscall.h"

#define USER_STACK_SIZE 4096

/* 用户态的栈。内核在任务 B 的 TSS 中把 ESP 设为它的末尾。 */
uint8_t user_stack[USER_STACK_SIZE] __attribute__((aligned(16)));

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
