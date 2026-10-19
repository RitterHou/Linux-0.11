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
#define SELFTEST_EARLY_KEY     10   /* 初始化 8259 之前就有按键：期望键盘之后仍能工作 */
#define SELFTEST_KEYBOARD_EDGE 11   /* 喂扫描码：Caps Lock 自动重复、退格跨过自动换行 */
#define SELFTEST_PAGE_ALLOC    12   /* 分配、释放、再分配物理页，再分光全部页，把结果写到第 7 行 */
#define SELFTEST_DOUBLE_FREE   13   /* 同一页释放两次：期望 Kernel panic */
#define SELFTEST_HD_RW         14   /* 内核任务 A 读写硬盘扇区，把结果写到第 7 行 */

/* 20 号起是用户态自检，由任务 B 在特权级 3 执行，见 user.c。 */
#define SELFTEST_USER_WRITE_KERNEL 20   /* 用户程序写内核的变量：期望因 #PF 被结束，错误码 7 */
#define SELFTEST_USER_PRIVILEGED   21   /* 用户程序执行 cli：期望因 #GP 被结束 */
#define SELFTEST_USER_BAD_POINTER  22   /* 用户程序把内核地址交给 write：期望返回 -1 */
#define SELFTEST_USER_EXIT         23   /* 用户程序调用 exit(7)：期望只剩任务 A 运行 */
#define SELFTEST_ALL_EXIT          24   /* 任务 A、B 都调用 exit：期望切回任务 0 空转 */
#define SELFTEST_USER_SPIN         25   /* 用户程序在用户态空转：测试在用户态收到 NMI */
#define SELFTEST_USER_WRITE_CODE   26   /* 用户程序改写自己的代码：期望因 #PF 被结束，错误码 7 */
#define SELFTEST_USER_DEEP_STACK   27   /* 用户程序用掉 5 页多的栈后 exit(3)：期望逐页补上，结束后全部收回 */
#define SELFTEST_USER_WILD_READ    28   /* 用户程序读栈区以外未映射的地址：期望不补页，因 #PF 被结束 */
#define SELFTEST_USER_FORK         29   /* fork 后父子各改栈上的变量：期望互不影响，两边的返回值正确 */
#define SELFTEST_USER_WAIT         30   /* waitpid：取退出码、内核写共享页要复制、任务槽用光、WNOHANG、被结束的子进程 */
#define SELFTEST_USER_FS           31   /* open/read/close：读文件、大文件、目录、子目录，描述符用光，fork 后共用读写位置 */
#define SELFTEST_USER_EXEC         32   /* exec：失败时原程序不受影响，hello 打印参数，check 检查自己的段，写代码页被结束 */
#define SELFTEST_USER_PIPE         33   /* pipe 和 dup：读写、写满后等待、写的一头全关后读到 0、读的一头关了写返回 -1 */
#define SELFTEST_USER_FSWRITE      34   /* 写文件：O_EXCL、读写权限、O_TRUNC、O_APPEND、删掉开着的文件、目录的错误、共用 i 节点 */
#define SELFTEST_USER_SIGNAL       35   /* kill 和 signal：结束、忽略、SIGKILL、进程组、setsid、打断管道和 waitpid、SIGPIPE、参数检查 */
#define SELFTEST_USER_BRK          36   /* brk：堆的起点和末尾、按需补页、堆和栈之间不补页、fork 继承、越界、exec 后从程序末尾开始 */
#define SELFTEST_USER_ALARM        37   /* alarm 和 pause：闹钟准时、剩几秒、取消、默认结束进程、fork 不继承、exec 保留 */
#define SELFTEST_USER_SYNC         38   /* 延迟写回：写文件不马上写盘、sync 写回、换块时写回、读回来的内容对 */
#define SELFTEST_USER_LINK         39   /* 硬链接：两个名字一个 i 节点、链接数、删一个名字不收回、错误情况 */
#define SELFTEST_USER_TIME         40   /* 时间：日期换算、time 往前走、文件和目录的修改时间、utime */
#define SELFTEST_USER_FCNTL        41   /* dup2、fcntl、close-on-exec */
#define SELFTEST_USER_NICE         42   /* nice 和 times：nice 值、继承、按 nice 分 CPU、utime 和 stime、cutime */
#define SELFTEST_USER_PERM         43   /* 用户和权限：用户号、读写执行、新建和删除、粘滞位、chmod、chown、kill、setuid 程序 */
#define SELFTEST_USER_STOP         44   /* 作业控制：停下、WUNTRACED、SIGCONT、拦不住的 SIGSTOP、孤儿进程组、setpgid */
#define SELFTEST_USER_SWAP         45   /* 交换：只留 300 页空闲物理页，用 2 MB 的堆、fork、两个一起用、exec，换出去的页读回来都对 */

#define SELFTEST_IN_USER(n) ((n) >= 20)

#endif
