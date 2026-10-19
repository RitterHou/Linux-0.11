/*
 * exec：把当前任务换成磁盘上的另一个程序，与 Linux 0.11 的 fs/exec.c 中的 do_execve() 对应。
 *
 * 程序是 ELF 格式（用 clang 和 ld.lld 编出来的就是），链接在用户空间的底部 0x40000000。步骤：
 *   1. 把参数从用户内存复制进内核：马上要扔掉旧的地址空间，它们会跟着消失。路径已由 sys_exec() 复制好。
 *   2. 检查有执行权限（root 也要至少有一个执行位），读 ELF 头和程序头，检查是 32 位 x86 的可执行文件、
 *      要装入的段都落在用户空间里。文件有 setuid 位的，第 5 步成功以后有效用户号换成文件的主人（setgid 位换组）。
 *   3. 建一份新的页目录，把每个要装入的段（PT_LOAD）一页页放进去：分一页，按物理地址把文件内容读进来，
 *      多出来的部分（.bss）保持为 0。代码段只读，数据段可写。
 *   4. 在栈顶摆好参数：字符串、argv 指针数组、argv、argc，最下面再放一个假的返回地址，
 *      于是程序的入口 _start(argc, argv) 可以写成一个普通的 C 函数。
 *   5. 都成功了，才换上新页目录、收回旧的，再改写栈上保存的现场：EIP 改成程序的入口，ESP 改成新栈顶。
 *      系统调用返回时 iret 就落到新程序的第一条指令上。中途失败就返回 -1，旧程序原封不动。
 * 文件开头是 #! 的是脚本：第一行写着解释器的路径，后面可以跟一个参数。那就改去装入解释器，参数排成
 * 解释器的名字、那个参数（有的话）、脚本的路径、原来的 argv[1] 以后。于是 #!/bin/sh 开头的脚本，
 * 运行起来就是 sh 脚本路径 参数…。解释器自己不能又是脚本。Linux 0.11 的 do_execve() 也是这样（tytso 加的），
 * 不同的是：它的第一行可以长到 1022 个字节，这里只有 127；它把行尾的空白留在参数里，这里去掉（后来的 Linux 也去掉）。
 * Linux 0.11 读的是 a.out 格式：程序的页在缺页时才从文件里读（do_no_page() 调用 bread_page()），
 * 参数放在栈顶的几页里，同样是改写栈上的 eip 和 esp，让系统调用“返回”到新程序。
 */
#include "kernel.h"
#include "fs.h"
#include "syscall.h"

#define MAX_ARGS      16
#define ARG_BYTES     1024          /* 参数字符串加起来最多这么长，放得进栈顶的一页 */
#define MAX_PHDRS     8

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

/*
 * 看文件是不是 #! 开头的脚本：是的话把解释器的路径放进 interp，重排参数（见文件开头的说明），返回 0；
 * 不是脚本，interp 留空，返回 0；#! 后面没有解释器、第一行太长、参数放不下，返回 -1。
 * 解释器不是 / 开头的，从当前目录找起，和别的路径一样（Linux 0.11 的 namei() 也从 current->pwd 找起）。
 */
static int script(const char *path, const struct minix_inode *ip, char *interp, char *args, uint32_t *used,
                  uint32_t *offsets, uint32_t *argc)
{
    char line[PATH_MAX];
    int32_t n = inode_read(ip, 0, line, sizeof(line) - 1);

    interp[0] = '\0';
    if (n < 2 || line[0] != '#' || line[1] != '!')
        return 0;
    line[n] = '\0';
    char *end = line + 2;
    while (*end && *end != '\n')
        end++;
    if (*end != '\n' && n == (int32_t)sizeof(line) - 1)
        return -1;                  /* 第一行太长：截短了，解释器的参数就不是写的那样了 */
    *end = '\0';
    while (end > line + 2 && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';             /* 行尾的空白和回车不要：Windows 写的脚本每行末尾有 \r */
    char *p = line + 2, *arg = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    char *start = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    if (p == start)
        return -1;                  /* #! 后面没有解释器 */
    if (*p) {                       /* 解释器后面的整段（到行尾）算一个参数 */
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        arg = p;
    }
    uint32_t ni = 0;                /* 先拼好解释器的路径：太长就失败，参数还没动 */
    if (start[0] != '/') {
        for (const char *c = current_cwd(); *c; c++)
            interp[ni++] = *c;
        if (interp[ni - 1] != '/')
            interp[ni++] = '/';
    }
    for (const char *c = start; *c; c++) {
        if (ni == PATH_MAX - 1)
            return -1;
        interp[ni++] = *c;
    }
    interp[ni] = '\0';
    const char *base = start;
    for (const char *q = start; *q; q++)
        if (*q == '/')
            base = q + 1;

    /* 新的参数：解释器的名字、arg、脚本的路径，再接上原来的 argv[1] 以后 */
    const char *front[3] = { base, arg, path };
    static char out[ARG_BYTES];     /* 内核栈只有一页，do_exec 已经用了 1 KB；exec 在文件系统锁里，同时只有一个 */
    uint32_t noff[MAX_ARGS], nargc = 0, nused = 0;
    for (int k = 0; k < 3 + (int)(*argc > 1 ? *argc - 1 : 0); k++) {
        const char *s = k < 3 ? front[k] : args + offsets[k - 3 + 1];
        if (!s)
            continue;
        uint32_t len = 0;
        while (s[len])
            len++;
        if (nargc == MAX_ARGS || nused + len + 1 > ARG_BYTES)
            return -1;
        noff[nargc++] = nused;
        for (uint32_t i = 0; i <= len; i++)
            out[nused + i] = s[i];
        nused += len + 1;
    }
    for (uint32_t i = 0; i < nused; i++)
        args[i] = out[i];
    for (uint32_t i = 0; i < nargc; i++)
        offsets[i] = noff[i];
    *argc = nargc;
    *used = nused;
    return 0;
}

/* 把一个要装入的段放进新的页目录 dir。 */
static int load_segment(uint32_t dir, const struct minix_inode *ip, const struct elf_phdr *ph)
{
    uint32_t end = ph->vaddr + ph->memsz;

    if (ph->filesz > ph->memsz || ph->vaddr < USER_LOW || end < ph->vaddr || end > STACK_LOW)
        return -1;
    for (uint32_t va = ph->vaddr & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        /* 和前一个段共用一页、可写属性又不一样：那一页没法同时又只读又可写，不装 */
        if (user_page_writable(dir, va) >= 0 && user_page_writable(dir, va) != !!(ph->flags & PF_W))
            return -1;
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

    /* 2. 文件：必须是普通文件，有执行权限，是 ELF 可执行文件；#! 开头的脚本换成它的解释器 */
    uint32_t ino;
    struct minix_inode ip;
    struct elf_header eh;
    struct elf_phdr ph[MAX_PHDRS];
    char interp[PATH_MAX];
    const char *name = path;        /* ps 里显示的名字：脚本显示脚本自己的名字 */
    if (namei(path, &ino, &ip) < 0 || (ip.mode & S_IFMT) != S_IFREG || !permission(&ip, MAY_EXEC))
        return -1;
    if (script(path, &ip, interp, args, &used, offsets, &argc) < 0)
        return -1;
    if (interp[0]) {                /* 脚本：从此 ip 是解释器的。脚本自己的 setuid 位不管用，只看解释器的，和 Linux 一样 */
        path = interp;
        if (namei(path, &ino, &ip) < 0 || (ip.mode & S_IFMT) != S_IFREG || !permission(&ip, MAY_EXEC))
            return -1;
    }
    if (inode_read(&ip, 0, &eh, sizeof(eh)) != sizeof(eh) || !elf_ok(&eh) ||
        eh.entry < USER_LOW || eh.entry >= STACK_LOW)
        return -1;
    uint32_t ph_bytes = eh.phnum * sizeof(ph[0]);
    if (inode_read(&ip, eh.phoff, ph, ph_bytes) != (int32_t)ph_bytes)
        return -1;

    /* 3. 新的地址空间 */
    uint32_t dir = new_page_dir();
    if (dir == 0)
        return -1;
    uint32_t end = USER_LOW;                /* 最高的段的末尾（.bss 的末尾）：堆从这里开始 */
    for (uint32_t i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        if (load_segment(dir, &ip, &ph[i]) < 0)
            goto fail;
        if (ph[i].vaddr + ph[i].memsz > end)
            end = ph[i].vaddr + ph[i].memsz;
    }
    /* 最高的段是只读的（程序没有数据段）：它最后一页里剩下的地方写不了，堆从下一页开始 */
    if ((end & (PAGE_SIZE - 1)) && user_page_writable(dir, end) != 1)
        end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

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
    sched_set_name(name);
    sched_exec_ids(ip.mode & S_ISUID, ip.uid, ip.mode & S_ISGID, ip.gid);   /* setuid 程序：以文件主人的身份运行 */
    sched_exec_signals();           /* 原来的处理函数在新程序里没有意义 */
    sched_set_brk(end);             /* 新程序的堆是空的，从它的 .bss 末尾开始 */
    if (first) {
        first = 0;
        vga_write_at(0, 11, "E", VGA_GRAY);     /* 第一次从硬盘装入程序 */
    }
    return 0;                       /* 作为 EAX 交给新程序 */

fail:
    free_page_dir(dir);
    return -1;
}
