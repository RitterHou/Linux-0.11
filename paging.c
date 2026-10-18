/*
 * 分页：建立页目录和页表，装入 CR3，再打开 CR0 的 PG 位。
 *
 * 打开分页后，CPU 访问的每个地址都是虚拟地址，要经过两级查表才得到物理地址：
 *   虚拟地址的高 10 位选页目录项 → 找到一张页表；中间 10 位选页表项 → 找到一个 4 KB 物理页；
 *   低 12 位是页内偏移。
 *
 * 这里把前 16 MB 一一对应映射（虚拟地址 = 物理地址），所以打开分页前后代码照常运行；
 * Linux 0.11 的 head.s 也是这样映射前 16 MB 的。另外有两处不同：
 *   - 第 0 页不映射，访问空指针会触发缺页异常，而不是悄悄读写物理地址 0；
 *   - 虚拟地址 0xC0000000 也映射到显存 0xB8000，同一块物理内存有了两个地址；
 *   - 只有用户程序所在的几页（链接脚本中的 __user_start～__user_end）打开“用户”位，
 *     特权级 3 的代码访问其他页都会触发缺页异常；其中代码所在的页对用户态只读。
 *
 * 用户栈不在这里预先映射：它在 0x40000000 起的 4 MB 里，用到哪一页才由缺页异常分配（handle_page_fault）。
 *
 * 这里建的是内核的页目录，任务 0 和内核任务用它。每个用户任务有一份自己的页目录（new_page_dir）：
 * 内核部分的页目录项照抄这一份，指向同样的页表，所以切换任务时内核的地址都不变；
 * 用户空间那一项各管各的。硬件任务切换会从 TSS 装入 CR3，换任务就换了页目录。
 */
#include "io.h"
#include "kernel.h"

#define PAGE_PRESENT  0x001         /* 存在位：为 0 时访问会触发 14 号缺页异常 */
#define PAGE_WRITE    0x002         /* 可写 */
#define PAGE_USER     0x004         /* 用户：为 0 时只有特权级 0～2 能访问 */
#define CR0_PG        0x80000000u
#define CR0_WP        0x00010000u   /* 写保护：特权级 0 写只读页也会触发缺页 */

/* 缺页异常错误码的低两位 */
#define PF_PROTECTION 0x1           /* 1：页存在但没权限；0：页不存在 */
#define PF_WRITE      0x2           /* 1：写操作 */

#define USER_PDE      (USER_LOW >> 22)    /* 用户空间是第 256 个页目录项 */
#define PAGE_COW      0x200         /* 页表项第 9 位，CPU 不用：标记 fork 时去掉了可写位、等着写时复制的页 */

#define ENTRIES       1024          /* 每张页目录或页表有 1024 项，每项 4 字节 */
#define IDENTITY_MB   16
#define IDENTITY_TABLES (IDENTITY_MB / 4)   /* 每张页表管 4 MB */

/* mem.c 按物理地址直接清零分出去的页，所以能分配的页都必须在一一对应映射的范围内。 */
_Static_assert(HIGH_MEM <= IDENTITY_MB * 0x100000u, "allocatable pages must be identity-mapped");

/* 都在 .bss，由 entry.asm 清零，所以没有填写的项存在位都是 0。CR3 要求 4 KB 对齐。 */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t identity_tables[IDENTITY_TABLES][ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t alias_table[ENTRIES] __attribute__((aligned(PAGE_SIZE)));

extern uint8_t __user_start[], __user_data[], __user_end[];   /* 链接脚本定义，都按 4 KB 对齐 */

void paging_init(void)
{
    /*
     * 页目录项和页表项都有用户位，CPU 取两者的“与”：页目录项打开用户位，
     * 才轮到页表项决定每一页。内核的页在页表项中不开用户位，用户态照样访问不了。
     */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        for (int i = 0; i < ENTRIES; i++) {
            uint32_t phys = (uint32_t)(t * ENTRIES + i) * PAGE_SIZE;
            identity_tables[t][i] = phys | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[t] = (uint32_t)identity_tables[t] | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    identity_tables[0][0] = 0;      /* 第 0 页不映射，用来捕获空指针 */

    /*
     * 用户程序的页打开用户位。它们在前 4 MB 内，属于第一张页表。
     * 代码页去掉可写位：用户栈溢出或野指针写到代码上，会立刻触发缺页，而不是悄悄改掉代码。
     * 内核也不能写它们：下面打开了 CR0.WP。
     */
    for (uint32_t addr = (uint32_t)__user_start; addr < (uint32_t)__user_end; addr += PAGE_SIZE) {
        uint32_t *pte = &identity_tables[0][addr / PAGE_SIZE];
        *pte |= PAGE_USER;
        if (addr < (uint32_t)__user_data)
            *pte &= ~PAGE_WRITE;
    }

    /* 0xC0000000 的高 10 位是 768，用第 768 个页目录项；它的第 0 页指向显存。 */
    alias_table[0] = VGA_PHYS | PAGE_PRESENT | PAGE_WRITE;
    page_directory[VGA_ALIAS >> 22] = (uint32_t)alias_table | PAGE_PRESENT | PAGE_WRITE;

    /* 页表在一一对应映射的范围内，它们的虚拟地址就是物理地址，可以直接装入 CR3。 */
    __asm__ volatile("movl %0, %%cr3" : : "r"(page_directory) : "memory");

    /*
     * 同时打开 WP：特权级 0 写只读页也会触发缺页。fork 之后父子共用的栈页是只读的，
     * 内核替用户写这些页（例如 waitpid 写回退出码）时也要先复制，否则会改到另一个进程的那一份。
     * 386 没有这一位，在特权级 0 不理会只读位，所以 Linux 0.11 在内核写用户内存之前
     * 先调用 verify_area() 预先处理写时复制；WP 是 486 加上的，QEMU 模拟的 CPU 有它。
     */
    uint32_t cr0;
    __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movl %0, %%cr0" : : "r"(cr0 | CR0_PG | CR0_WP) : "memory");
}

/* [addr, addr + size) 是否完全落在 [start, end) 内，加法溢出也不会算错。 */
static int range_within(uint32_t addr, uint32_t size, uint32_t start, uint32_t end)
{
    return addr >= start && addr <= end && size <= end - addr;
}

/*
 * 把以 0 结尾的字符串从 src 复制到 dst，最多 max 个字节（含结尾的 0），返回长度；太长返回 -1。
 * from_user 时 src 是用户给的地址：一个字节一个字节地检查是否在用户的内存里，再读。
 * 复制进来以后，内核只和自己的这份打交道。Linux 0.11 用 get_fs_byte() 逐个字节读用户段里的字符串。
 */
int copy_string(char *dst, uint32_t src, uint32_t max, int from_user)
{
    for (uint32_t i = 0; i < max; i++) {
        if (from_user && !user_range_ok(src + i, 1))
            return -1;
        dst[i] = ((const char *)src)[i];
        if (dst[i] == '\0')
            return (int)i;
    }
    return -1;
}

/* 用户空间正好是一个页目录项管的 4 MB：缺页时只用分配这一张页表，结束时只用收回这一张。 */
_Static_assert(USER_LOW % 0x400000u == 0 && USER_TOP - USER_LOW == 0x400000u,
               "user stack area must be exactly one page table");

uint32_t page_faults;               /* 缺页异常补上了几页（页不存在），显示在第 3 行 */
uint32_t cow_faults;                /* 写时复制处理了几次（写了 fork 留下的只读页） */

uint32_t kernel_page_dir(void)
{
    return (uint32_t)page_directory;
}

/* 当前的页目录：CR3 里是它的物理地址，页目录都在一一对应映射的范围内，物理地址也就是虚拟地址。 */
static uint32_t *current_page_dir(void)
{
    uint32_t cr3;
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
    return (uint32_t *)cr3;
}

/* 重新装入 CR3，丢掉 TLB 里缓存的翻译。改了存在的页表项之后要这样做；386 没有只刷一页的 invlpg。 */
static void flush_tlb(void)
{
    __asm__ volatile("movl %0, %%cr3" : : "r"(current_page_dir()) : "memory");
}

/*
 * 给一个用户任务建页目录：分一页，照抄内核的页目录。内核部分的项指向同样的页表，
 * 用户空间那一项在内核的页目录里是空的，复制过来也是空的。没有空闲页时返回 0。
 */
uint32_t new_page_dir(void)
{
    uint32_t dir = get_free_page();

    if (dir != 0)
        for (int i = 0; i < ENTRIES; i++)
            ((uint32_t *)dir)[i] = page_directory[i];
    return dir;
}

/*
 * fork：给子进程建页目录。内核部分照抄；用户空间的页父子共用：两边的页表项都去掉可写位，
 * 物理页的引用次数加一，谁先写，谁就在写时复制里得到自己的一份。页表本身不共用，子进程有自己的一张。
 * 本来就只读的页（程序的代码）不打写时复制的记号，写它仍是错误。
 * 父进程自己的页表项被改成了只读，TLB 里可能还缓存着可写的旧翻译，所以要刷新。
 * Linux 0.11 的 copy_page_tables() 做同样的事。没有空闲页时返回 0。
 */
uint32_t copy_page_dir(uint32_t src)
{
    uint32_t dir = new_page_dir();
    if (dir == 0)
        return 0;

    uint32_t pde = ((uint32_t *)src)[USER_PDE];
    if (pde & PAGE_PRESENT) {
        uint32_t table = get_free_page();
        if (table == 0) {
            free_page(dir);
            return 0;
        }
        uint32_t *from = (uint32_t *)(pde & ~0xFFFu), *to = (uint32_t *)table;
        for (int i = 0; i < ENTRIES; i++) {
            if (from[i] & PAGE_PRESENT) {
                if (from[i] & PAGE_WRITE)
                    from[i] = (from[i] & ~PAGE_WRITE) | PAGE_COW;
                to[i] = from[i];
                page_add_ref(from[i] & ~0xFFFu);
            }
        }
        ((uint32_t *)dir)[USER_PDE] = table | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    flush_tlb();
    return dir;
}

/*
 * 写时复制：写了一个只读的栈页。还有别的任务在用这一页，就分一页、把内容复制过去，换成自己的，
 * 旧页的引用次数减一；只剩自己在用（另一方已经复制走了，或者已经结束），直接改回可写，不用复制。
 * 改的是存在的页表项，要刷新 TLB。Linux 0.11 的 do_wp_page() / un_wp_page() 也是这样。
 */
static int copy_on_write(uint32_t *pte)
{
    uint32_t old = *pte & ~0xFFFu;

    if (page_ref_count(old) == 1) {
        *pte = (*pte | PAGE_WRITE) & ~PAGE_COW;
    } else {
        uint32_t page = get_free_page();
        if (page == 0)
            return 0;
        for (int i = 0; i < PAGE_SIZE / 4; i++)
            ((uint32_t *)page)[i] = ((uint32_t *)old)[i];
        *pte = page | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        free_page(old);
    }
    flush_tlb();
    if (cow_faults++ == 0)
        vga_write_at(0, 10, "W", VGA_GRAY);     /* 第一次写时复制 */
    mem_show();
    return 1;
}

/*
 * 缺页异常（14 号）：出错的地址在用户空间、而且是“页不存在”（错误码第 0 位为 0），
 * 就分配一页清零的物理页映射上去，返回 1；中断返回后 CPU 重新执行出错的那条指令，这次就能访问了。
 * 写了 fork 留下的只读栈页，交给 copy_on_write()。
 * 别的情况（页存在但没有权限、地址不在栈区、内存用完了）返回 0，由调用者照原来的规则处理：
 * 用户程序出错就结束它，内核出错就显示异常面板。
 * 内核读写用户传来的缓冲区时也可能碰到栈区里还没映射的页，同样在这里补上。
 * 页表本身也是按需分配的：第一次缺页时，第 256 个页目录项还是空的。
 * Linux 0.11 的 do_no_page() 也是这样，用 get_free_page() 分一页、put_page() 填进页表；
 * 它还会先试着从可执行文件里读入那一页，或者与正在运行同一程序的进程共用，这里只有清零的页。
 */
int handle_page_fault(uint32_t addr, uint32_t error_code)
{
    if (addr < USER_LOW || addr >= USER_TOP)
        return 0;
    if (current_page_dir() == page_directory)   /* 内核的页目录里没有用户空间，也不能给它建 */
        return 0;

    uint32_t *pde = &current_page_dir()[addr >> 22];   /* 当前任务自己的用户空间 */
    if (error_code & PF_PROTECTION) {
        /* 页存在却出错：只有“写一个打了写时复制记号的页”能处理，那是 fork 留下的共享页 */
        if (!(error_code & PF_WRITE) || !(*pde & PAGE_PRESENT))
            return 0;
        uint32_t *pte = &((uint32_t *)(*pde & ~0xFFFu))[(addr >> 12) & (ENTRIES - 1)];
        if ((*pte & (PAGE_PRESENT | PAGE_WRITE | PAGE_COW)) != (PAGE_PRESENT | PAGE_COW))
            return 0;
        return copy_on_write(pte);
    }

    if (!(*pde & PAGE_PRESENT)) {
        uint32_t table = get_free_page();       /* 已清零：1024 项都不存在 */
        if (table == 0)
            return 0;
        *pde = table | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    uint32_t page = get_free_page();
    if (page == 0)
        return 0;
    uint32_t *table = (uint32_t *)(*pde & ~0xFFFu);
    table[(addr >> 12) & (ENTRIES - 1)] = page | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    /* 不用刷新 TLB：CPU 不缓存“不存在”的页表项，重新执行那条指令时会重新查表。 */

    if (page_faults++ == 0)
        vga_write_at(0, 9, "F", VGA_GRAY);      /* 第一次补上缺页 */
    mem_show();
    return 1;
}

/*
 * 系统调用碰用户内存之前，检查 [addr, addr + size) 是否都在用户自己的内存里，并且现在就把每一页准备好：
 * 还没映射的页当场补上；要写的话，写时复制的页当场复制好。准备不了（不在用户的内存里、要写只读的代码页、
 * 内存不够）就返回 0，系统调用返回 -1。准备好以后，内核读写这段内存就不会再缺页：
 * 要是等到复制数据时才缺页、才发现页是只读的或者内存不够，内核已经在特权级 0 里，只能停机。
 * Linux 0.11 的 verify_area() 也是在内核写用户内存之前，先把写时复制的页处理好。
 * 一个任务的页表只有它自己会改，准备好之后到用完之前不会有别人把页拿走。
 */
static int prepare_user(uint32_t addr, uint32_t size, int write)
{
    if (range_within(addr, size, (uint32_t)__user_start, (uint32_t)__user_end))
        return !write || addr >= (uint32_t)__user_data;     /* 内核映像里的用户程序：代码只读，一直映射着 */
    if (!range_within(addr, size, USER_LOW, USER_TOP))
        return 0;
    for (uint32_t page = addr & ~(PAGE_SIZE - 1); page < addr + size; page += PAGE_SIZE) {
        uint32_t flags = irq_save();
        uint32_t *dir = current_page_dir();
        uint32_t pde = dir[page >> 22];
        uint32_t pte = (pde & PAGE_PRESENT) ? ((uint32_t *)(pde & ~0xFFFu))[(page >> 12) & (ENTRIES - 1)] : 0;
        int ok = 1;
        if (!(pte & PAGE_PRESENT))
            ok = handle_page_fault(page, write ? PF_WRITE : 0);
        else if (write && !(pte & PAGE_WRITE))
            ok = handle_page_fault(page, PF_PROTECTION | PF_WRITE);     /* 写时复制的页复制好；代码页失败 */
        irq_restore(flags);
        if (!ok)
            return 0;
    }
    return 1;
}

/* 内核要读用户给的 [addr, addr + size)。 */
int user_range_ok(uint32_t addr, uint32_t size)
{
    return prepare_user(addr, size, 0);
}

/* 内核要往用户给的 [addr, addr + size) 写。 */
int user_write_ok(uint32_t addr, uint32_t size)
{
    return prepare_user(addr, size, 1);
}

/*
 * 在页目录 dir 的用户空间里给 addr 所在的页准备一个物理页（清零的），返回它的物理地址；没有内存时返回 0。
 * 这一页已经有了就直接用它，需要可写时打开可写位。exec 用它往还没装入 CR3 的新页目录里放程序：
 * 物理页在一一对应映射的范围内，内核按物理地址直接写。页表不存在时先分一页当页表。
 */
uint32_t map_user_page(uint32_t dir, uint32_t addr, int writable)
{
    uint32_t *pde = &((uint32_t *)dir)[addr >> 22];
    if (!(*pde & PAGE_PRESENT)) {
        uint32_t table = get_free_page();
        if (table == 0)
            return 0;
        *pde = table | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    uint32_t *pte = &((uint32_t *)(*pde & ~0xFFFu))[(addr >> 12) & (ENTRIES - 1)];
    if (*pte & PAGE_PRESENT) {
        if (writable)
            *pte |= PAGE_WRITE;
        return *pte & ~0xFFFu;
    }
    uint32_t page = get_free_page();
    if (page == 0)
        return 0;
    *pte = page | PAGE_PRESENT | PAGE_USER | (writable ? PAGE_WRITE : 0);
    return page;
}

/* 页目录 dir 里 addr 所在的页：没有映射返回 -1，可写返回 1，只读返回 0。 */
int user_page_writable(uint32_t dir, uint32_t addr)
{
    uint32_t pde = ((uint32_t *)dir)[addr >> 22];
    if (!(pde & PAGE_PRESENT))
        return -1;
    uint32_t pte = ((uint32_t *)(pde & ~0xFFFu))[(addr >> 12) & (ENTRIES - 1)];
    if (!(pte & PAGE_PRESENT))
        return -1;
    return (pte & PAGE_WRITE) ? 1 : 0;
}

/*
 * 用户任务结束时，收回它的用户空间的每一页、那张页表和页目录本身。
 * 调用者马上要切到别的任务，换 CR3 时 TLB 里这些页的旧翻译也随之作废，这里不用另外刷新。
 * Linux 0.11 的 do_exit() 调用 free_page_tables() 做同样的事。
 */
void free_page_dir(uint32_t dir)
{
    uint32_t pde = ((uint32_t *)dir)[USER_LOW >> 22];

    if (pde & PAGE_PRESENT) {
        uint32_t *table = (uint32_t *)(pde & ~0xFFFu);
        for (int i = 0; i < ENTRIES; i++)
            if (table[i] & PAGE_PRESENT)
                free_page(table[i] & ~0xFFFu);
        free_page(pde & ~0xFFFu);
    }
    free_page(dir);
}
