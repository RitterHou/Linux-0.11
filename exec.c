/*
 * exec：把当前任务换成磁盘上的另一个程序，与 Linux 0.11 的 fs/exec.c 中的 do_execve() 对应。
 *
 * 程序是 ELF 格式（用 clang 和 ld.lld 编出来的就是），链接在用户空间的底部 0x40000000。步骤：
 *   1. 把参数从用户内存复制进内核：马上要扔掉旧的地址空间，它们会跟着消失。路径已由 sys_exec() 复制好。
 *   2. 读 ELF 头和程序头，检查是 32 位 x86 的可执行文件、要装入的段都落在用户空间里。
 *   3. 建一份新的页目录，把每个要装入的段（PT_LOAD）一页页放进去：分一页，按物理地址把文件内容读进来，
 *      多出来的部分（.bss）保持为 0。代码段只读，数据段可写。
 *   4. 在栈顶摆好参数：字符串、argv 指针数组、argv、argc，最下面再放一个假的返回地址，
 *      于是程序的入口 _start(argc, argv) 可以写成一个普通的 C 函数。
 *   5. 都成功了，才换上新页目录、收回旧的，再改写栈上保存的现场：EIP 改成程序的入口，ESP 改成新栈顶。
 *      系统调用返回时 iret 就落到新程序的第一条指令上。中途失败就返回 -1，旧程序原封不动。
 * Linux 0.11 读的是 a.out 格式：程序的页在缺页时才从文件里读（do_no_page() 调用 bread_page()），
 * 参数放在栈顶的几页里，同样是改写栈上的 eip 和 esp，让系统调用“返回”到新程序。
 */
#include "kernel.h"
#include "fs.h"

#define MAX_ARGS      16
#define ARG_BYTES     1024          /* 参数字符串加起来最多这么长，放得进栈顶的一页 */
#define MAX_PHDRS     8
#define STACK_RESERVE (64 * 1024)   /* 程序最高只能到用户空间顶端往下 64 KB，上面留给栈 */

struct elf_header {
    uint8_t ident[16];              /* 0x7F 'E' 'L' 'F'，1 表示 32 位，1 表示小端 */
    uint16_t type, machine;         /* 2：可执行文件；3：x86 */
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));

#define PT_LOAD 1                   /* 要装入内存的段 */
#define PF_W    2                   /* 段可写 */

static int elf_ok(const struct elf_header *eh)
{
    return eh->ident[0] == 0x7F && eh->ident[1] == 'E' && eh->ident[2] == 'L' && eh->ident[3] == 'F' &&
           eh->ident[4] == 1 && eh->ident[5] == 1 && eh->type == 2 && eh->machine == 3 &&
           eh->phentsize == sizeof(struct elf_phdr) && eh->phnum <= MAX_PHDRS;
}

/* 把一个要装入的段放进新的页目录 dir。 */
static int load_segment(uint32_t dir, const struct minix_inode *ip, const struct elf_phdr *ph)
{
    uint32_t end = ph->vaddr + ph->memsz;

    if (ph->filesz > ph->memsz || ph->vaddr < USER_LOW || end < ph->vaddr || end > USER_TOP - STACK_RESERVE)
        return -1;
    for (uint32_t va = ph->vaddr & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        uint32_t page = map_user_page(dir, va, ph->flags & PF_W);
        if (page == 0)
            return -1;
        /* 这一页里属于文件内容的部分：[lo, hi)；其余是 .bss 或页内的空隙，分页时已经清零 */
        uint32_t lo = va > ph->vaddr ? va : ph->vaddr;
        uint32_t hi = va + PAGE_SIZE < ph->vaddr + ph->filesz ? va + PAGE_SIZE : ph->vaddr + ph->filesz;
        if (lo < hi && inode_read(ip, ph->offset + (lo - ph->vaddr), (void *)(page + (lo - va)), hi - lo) !=
                           (int32_t)(hi - lo))
            return -1;
    }
    return 0;
}

int do_exec(const char *path, uint32_t user_argv, struct interrupt_frame *frame)
{
    static int first = 1;
    char args[ARG_BYTES];
    uint32_t offsets[MAX_ARGS];
    uint32_t argc = 0, used = 0;

    /* 1. 参数：argv 是一个以 0 结尾的指针数组，每个指针指向一个字符串，都在用户内存里 */
    while (user_argv) {
        if (!user_range_ok(user_argv + 4 * argc, 4))
            return -1;
        uint32_t p = ((const uint32_t *)user_argv)[argc];
        if (p == 0)
            break;
        if (argc == MAX_ARGS)
            return -1;
        int len = copy_string(args + used, p, ARG_BYTES - used, 1);
        if (len < 0)
            return -1;
        offsets[argc++] = used;
        used += (uint32_t)len + 1;
    }

    /* 2. 文件：必须是普通文件，有执行权限，是 ELF 可执行文件 */
    uint32_t ino;
    struct minix_inode ip;
    struct elf_header eh;
    struct elf_phdr ph[MAX_PHDRS];
    if (namei(path, &ino, &ip) < 0 || (ip.mode & S_IFMT) != S_IFREG || !(ip.mode & 0111))
        return -1;
    if (inode_read(&ip, 0, &eh, sizeof(eh)) != sizeof(eh) || !elf_ok(&eh) ||
        eh.entry < USER_LOW || eh.entry >= USER_TOP - STACK_RESERVE)
        return -1;
    uint32_t ph_bytes = eh.phnum * sizeof(ph[0]);
    if (inode_read(&ip, eh.phoff, ph, ph_bytes) != (int32_t)ph_bytes)
        return -1;

    /* 3. 新的地址空间 */
    uint32_t dir = new_page_dir();
    if (dir == 0)
        return -1;
    for (uint32_t i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].memsz != 0 && load_segment(dir, &ip, &ph[i]) < 0)
            goto fail;

    /* 4. 栈顶这一页：从上往下依次是参数字符串、argv[argc] = 0、argv[0..argc-1]、argv、argc、假的返回地址 */
    uint32_t top = USER_TOP - PAGE_SIZE;
    uint32_t stack = map_user_page(dir, top, 1);
    if (stack == 0)
        goto fail;
#define AT(va) ((uint32_t *)(stack + ((va) - top)))     /* 栈顶这一页里虚拟地址 va 对应的内核地址 */
    uint32_t strings = USER_TOP - used;
    for (uint32_t i = 0; i < used; i++)
        ((char *)stack)[strings - top + i] = args[i];
    uint32_t sp = strings & ~3u;
    sp -= 4 * (argc + 1);
    uint32_t argv = sp;
    for (uint32_t i = 0; i < argc; i++)
        *AT(argv + 4 * i) = strings + offsets[i];
    *AT(argv + 4 * argc) = 0;
    sp -= 4, *AT(sp) = argv;
    sp -= 4, *AT(sp) = argc;
    sp -= 4, *AT(sp) = 0;
#undef AT

    /* 5. 换上新的地址空间，收回旧的，改写返回现场 */
    free_page_dir(sched_set_page_dir(dir));
    frame->eip = eh.entry;
    frame->user_esp = sp;
    frame->eflags = 0x202;          /* IF=1，其余标志清零 */
    frame->ebx = frame->ecx = frame->edx = frame->esi = frame->edi = frame->ebp = 0;
    if (first) {
        first = 0;
        vga_write_at(0, 11, "E", VGA_GRAY);     /* 第一次从硬盘装入程序 */
    }
    return 0;                       /* 作为 EAX 交给新程序 */

fail:
    free_page_dir(dir);
    return -1;
}
