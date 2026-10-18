/*
 * 自检：make test 会用 SELFTEST=名称 另外构建几个内核，在打开中断之后故意制造异常，
 * 检验异常处理是否正确。正常构建时没有定义 SELFTEST，selftest_run() 什么也不做。
 */
#include "io.h"
#include "kernel.h"
#include "selftest.h"
#include "syscall.h"


#if defined(SELFTEST) && (SELFTEST == SELFTEST_SYSCALLS || SELFTEST == SELFTEST_PAGE_ALLOC)
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
    vga_write_at(0, 9, "Z", VGA_GRAY);   /* 清零完成的标记 */
#elif SELFTEST == SELFTEST_OTHER_SEGMENTS
    /*
     * 在 GDT 第 9 项（选择子 0x48）建一个基址为 1 MB 的数据段，把 DS、ES、FS、GS 换成它，
     * 然后只用 hlt 空转。中断处理代码若不换回内核数据段，访问的每个地址都会偏移 1 MB：
     * 显存写到了别处，屏幕上的 ticks 不再变化。换了但不恢复，空闲时看到的 DS 就会变成 0x10。
     * （不用空选择子测试：QEMU 的 TCG 模拟不检查通过空选择子访问内存。）
     */
    vga_write_at(0, 9, "N", VGA_GRAY);
    gdt_set_entry(9, 0x100000, 0xFFFFF, 0x92, 0xC);
    __asm__ volatile(
        "movw $0x48, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n"
        "1:\thlt\n\t"
        "jmp 1b" ::: "eax", "memory");
#elif SELFTEST == SELFTEST_BIG_KERNEL
    const volatile uint8_t *data = big_data;
    vga_write_at(0, 9, data[BIG_SIZE - 1] == 'Y' ? "Y" : "X", VGA_GRAY);
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
#elif SELFTEST == SELFTEST_PAGE_ALLOC
    /*
     * 分两页；把第一页写脏后释放，再分一页：应当拿回同一页，而且已经清零。
     * 然后一直分到分不出来，数一数一共几页，最后把 1 MB～16 MB 的每一页都还回去。
     */
    uint32_t before = free_page_count();
    uint32_t first = get_free_page(), second = get_free_page();
    volatile uint32_t *dirty = (volatile uint32_t *)first;
    dirty[0] = 0x12345678;
    dirty[PAGE_SIZE / 4 - 1] = 0x9ABCDEF0;
    free_page(first);
    uint32_t again = get_free_page();
    volatile uint32_t *p = (volatile uint32_t *)again;
    uint32_t zeroed = p[0] == 0 && p[PAGE_SIZE / 4 - 1] == 0;
    free_page(again);
    free_page(second);
    uint32_t total = 0;
    while (get_free_page() != 0)
        total++;
    uint32_t left = free_page_count();
    for (uint32_t addr = LOW_MEM; addr < HIGH_MEM; addr += PAGE_SIZE)
        free_page(addr);

    report("free", before, 10);
    report("p1", first, 16);
    report("p2", second, 16);
    report("p3", again, 16);
    report("zero", zeroed, 10);
    report("all", total, 10);
    report("left", left, 10);
    report("back", free_page_count(), 10);
#elif SELFTEST == SELFTEST_DOUBLE_FREE
    uint32_t page = get_free_page();
    free_page(page);
    free_page(page);                        /* 期望在这里 panic */
    vga_write_at(0, 9, "X", VGA_GRAY);      /* 不应该走到这里 */
#elif SELFTEST == SELFTEST_KEYBOARD_EDGE
    /*
     * 直接喂扫描码（第 1 套）：Caps Lock 按下两次（第二次是按住不放的重复）再松开，打 a；
     * 再按一次 Caps Lock 关掉，打 a；然后输入到超出一行，退格跨过行首，再打 ok。
     */
    static const uint8_t codes[] = { 0x3A, 0x3A, 0xBA, 0x1E, 0x3A, 0xBA, 0x1E };
    for (unsigned i = 0; i < sizeof(codes); i++)
        keyboard_process(codes[i]);
    for (int i = 0; i < 70; i++)
        keyboard_process(0x2D);             /* x */
    for (int i = 0; i < 4; i++)
        keyboard_process(0x0E);             /* 退格 */
    keyboard_process(0x18);                 /* o */
    keyboard_process(0x25);                 /* k */
#elif SELFTEST == SELFTEST_EARLY_KEY
    /* 在 selftest_early() 中完成 */
#elif SELFTEST_IN_USER(SELFTEST)
    /* 用户态的自检写在 user.c 中，这里不用做什么 */
#else
#error "未知的 SELFTEST"
#endif
}

/*
 * 在 pic_init() 之前调用。自检 EARLY_KEY：显示 W，一直等到键盘控制器里有一个字节，
 * 但不读走它，模拟开中断之前就有人按键。
 */
void selftest_early(void)
{
#if defined(SELFTEST) && SELFTEST == SELFTEST_EARLY_KEY
    vga_write_at(0, 9, "W", VGA_GRAY);
    while (!(inb(0x64) & 0x01))
        ;
#endif
}
