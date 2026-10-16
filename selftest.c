/*
 * 自检：make test 会用 SELFTEST=名称 另外构建几个内核，在打开中断之后故意制造异常，
 * 检验异常处理是否正确。正常构建时没有定义 SELFTEST，selftest_run() 什么也不做。
 */
#include "kernel.h"

#define SELFTEST_DIVIDE_ERROR 1     /* 除以零：期望 0 号 #DE */
#define SELFTEST_BAD_GATE     2     /* int 0x30：IDT 第 48 项为空，期望 13 号 #GP */
#define SELFTEST_UNMAPPED     3     /* 读未映射的 0xD0000000：期望 14 号 #PF，错误码 0 */
#define SELFTEST_NULL_WRITE   4     /* 写空指针：第 0 页未映射，期望 14 号 #PF，错误码 2 */

void selftest_run(void)
{
#if !defined(SELFTEST)
    /* 正常构建：什么也不做 */
#elif SELFTEST == SELFTEST_DIVIDE_ERROR
    /* 必须用汇编：C 的 1 / zero 会被 clang 优化成不含除法指令的比较，不会触发异常 */
    __asm__ volatile("xorl %%edx, %%edx; movl $1, %%eax; xorl %%ecx, %%ecx; divl %%ecx"
                     ::: "eax", "ecx", "edx");
#elif SELFTEST == SELFTEST_BAD_GATE
    __asm__ volatile("int $0x30");
#elif SELFTEST == SELFTEST_UNMAPPED
    /* 用汇编访问：C 中读写不存在的地址是未定义行为，编译器可能把它改写掉 */
    __asm__ volatile("movl 0xD0000000, %%eax" ::: "eax");
#elif SELFTEST == SELFTEST_NULL_WRITE
    __asm__ volatile("movl $1, 0x0" ::: "memory");
#else
#error "未知的 SELFTEST"
#endif
}
