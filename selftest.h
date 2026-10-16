/*
 * 自检编号：make test 用 SELFTEST=名称 构建自检内核，编译时得到 -DSELFTEST=SELFTEST_名称。
 * 内核态的自检写在 selftest.c 中，用户态的写在 user.c 中，两边共用这里的编号。
 */
#ifndef SELFTEST_H
#define SELFTEST_H

#define SELFTEST_DIVIDE_ERROR 1     /* 除以零：期望 0 号 #DE */
#define SELFTEST_BAD_GATE     2     /* int 0x30：IDT 第 48 项为空，期望 13 号 #GP */
#define SELFTEST_UNMAPPED     3     /* 读未映射的 0xD0000000：期望 14 号 #PF，错误码 0 */
#define SELFTEST_NULL_WRITE   4     /* 写空指针：第 0 页未映射，期望 14 号 #PF，错误码 2 */
#define SELFTEST_CLOBBER_LOADER 5   /* 清零 loader 所在内存：内核有自己的 GDT，期望照常运行 */
#define SELFTEST_OTHER_SEGMENTS 6   /* DS 等换成基址 1 MB 的段后空转：中断入口要换段并恢复 */
#define SELFTEST_BIG_KERNEL     7   /* 内核撑到约 90 个扇区，跨越多个磁道和柱面：期望完整读入 */
#define SELFTEST_REGISTER_DUMP  8   /* 寄存器设为已知值后执行 ud2：期望 6 号 #UD，面板显示这些值 */
#define SELFTEST_SYSCALLS       9   /* 依次调用各个系统调用，把返回值写到第 7 行 */

/* 20 号起是用户态自检，由任务 B 在特权级 3 执行，见 user.c。 */
#define SELFTEST_USER_WRITE_KERNEL 20   /* 用户程序写内核的变量：期望 14 号 #PF，错误码 7 */
#define SELFTEST_USER_PRIVILEGED   21   /* 用户程序执行 cli：期望 13 号 #GP */
#define SELFTEST_USER_BAD_POINTER  22   /* 用户程序把内核地址交给 write：期望返回 -1 */

#define SELFTEST_IN_USER(n) ((n) >= 20)

#endif
