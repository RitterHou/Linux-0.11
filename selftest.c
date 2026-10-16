/*
 * 自检：make test 会用 SELFTEST=名称 另外构建几个内核，在打开中断之后故意制造异常，
 * 检验异常处理是否正确。正常构建时没有定义 SELFTEST，selftest_run() 什么也不做。
 */
#include "kernel.h"
#include "syscall.h"

#define SELFTEST_DIVIDE_ERROR 1     /* 除以零：期望 0 号 #DE */
#define SELFTEST_BAD_GATE     2     /* int 0x30：IDT 第 48 项为空，期望 13 号 #GP */
#define SELFTEST_UNMAPPED     3     /* 读未映射的 0xD0000000：期望 14 号 #PF，错误码 0 */
#define SELFTEST_NULL_WRITE   4     /* 写空指针：第 0 页未映射，期望 14 号 #PF，错误码 2 */
#define SELFTEST_CLOBBER_LOADER 5   /* 清零 loader 所在内存：内核有自己的 GDT，期望照常运行 */
#define SELFTEST_OTHER_SEGMENTS 6   /* DS 等换成基址 1 MB 的段后空转：中断入口要换段并恢复 */
#define SELFTEST_BIG_KERNEL     7   /* 内核撑到约 90 个扇区，跨越多个磁道和柱面：期望完整读入 */
#define SELFTEST_REGISTER_DUMP  8   /* 寄存器设为已知值后执行 ud2：期望 6 号 #UD，面板显示这些值 */
#define SELFTEST_SYSCALLS       9   /* 依次调用各个系统调用，把返回值写到第 7 行 */

#if defined(SELFTEST) && SELFTEST == SELFTEST_SYSCALLS
static int report_col;

/* 在第 7 行接着写 "名字=值 "；base 为 16 时写 8 位十六进制。 */
static void report(const char *name, uint32_t value, unsigned base)
{
    char buf[12];
    const char *parts[3] = { name, "=", format_uint(value, base, buf) };

    for (int i = 0; i < 3; i++) {
        vga_write_at(7, report_col, parts[i], VGA_GRAY);
        for (const char *p = parts[i]; *p; p++)
            report_col++;
    }
    report_col++;
}
#endif

#if defined(SELFTEST) && SELFTEST == SELFTEST_BIG_KERNEL
/*
 * 40 KB 的只读数据会原样写进 kernel.bin。最后一个字节是标记，没读全就看不到它。
 * 编译器能在编译时算出这个字节是 'Y'，会把比较和整个数组都优化掉，
 * 所以加上 used 属性，并通过 volatile 指针读取。
 */
#define BIG_SIZE (40 * 1024)
static const uint8_t big_data[BIG_SIZE] __attribute__((used)) = { [BIG_SIZE - 1] = 'Y' };
#endif

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
#elif SELFTEST == SELFTEST_CLOBBER_LOADER
    /* 0x8000～0x81FF 是 loader.bin，其中有它的 GDT。之后每次中断都要按选择子查 GDT。 */
    for (volatile uint8_t *p = (volatile uint8_t *)0x8000; p < (volatile uint8_t *)0x8200; p++)
        *p = 0;
    vga_write_at(0, 8, "Z", VGA_GRAY);   /* 清零完成的标记 */
#elif SELFTEST == SELFTEST_OTHER_SEGMENTS
    /*
     * 在 GDT 第 7 项（选择子 0x38）建一个基址为 1 MB 的数据段，把 DS、ES、FS、GS 换成它，
     * 然后只用 hlt 空转。中断处理代码若不换回内核数据段，访问的每个地址都会偏移 1 MB：
     * 显存写到了别处，屏幕上的 ticks 不再变化。换了但不恢复，空闲时看到的 DS 就会变成 0x10。
     * （不用空选择子测试：QEMU 的 TCG 模拟不检查通过空选择子访问内存。）
     */
    vga_write_at(0, 8, "N", VGA_GRAY);
    gdt_set_entry(7, 0x100000, 0xFFFFF, 0x92, 0xC);
    __asm__ volatile(
        "movw $0x38, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n"
        "1:\thlt\n\t"
        "jmp 1b" ::: "eax", "memory");
#elif SELFTEST == SELFTEST_BIG_KERNEL
    const volatile uint8_t *data = big_data;
    vga_write_at(0, 8, data[BIG_SIZE - 1] == 'Y' ? "Y" : "X", VGA_GRAY);
#elif SELFTEST == SELFTEST_REGISTER_DUMP
    /*
     * 不声明被改写的寄存器：ud2 之后不会再回到这里，编译器之后的假设无关紧要。
     * 全局标号 selftest_ud2 让测试能查到 ud2 的地址，与面板上的 EIP 比较。
     */
    __asm__ volatile(
        "movl $0x11111111, %eax\n\t"
        "movl $0x22222222, %ebx\n\t"
        "movl $0x33333333, %ecx\n\t"
        "movl $0x44444444, %edx\n\t"
        "movl $0x55555555, %esi\n\t"
        "movl $0x66666666, %edi\n\t"
        "movl $0x77777777, %ebp\n"
        ".globl selftest_ud2\n"
        "selftest_ud2:\n\t"
        "ud2");
    __builtin_unreachable();
#elif SELFTEST == SELFTEST_SYSCALLS
    /* kmain 是任务 0，此时还没开始调度。write 的内容出现在第 6 行开头。 */
    int32_t pid = getpid();
    int32_t unknown = syscall3(99, 0, 0, 0);
    int32_t written = write(1, "xyz", 3);
    int32_t bad_fd = write(5, "q", 1);
    uint32_t before = timer_ticks;
    sleep(10);
    uint32_t slept = timer_ticks - before;

    report("pid", (uint32_t)pid, 10);
    report("unknown", (uint32_t)unknown, 16);
    report("write", (uint32_t)written, 10);
    report("badfd", (uint32_t)bad_fd, 16);
    report("slept", slept, 10);
#else
#error "未知的 SELFTEST"
#endif
}
