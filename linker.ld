/*
 * 内核链接脚本：决定各段在内存中的地址和顺序。
 * loader 把 kernel.bin 原样读到 0x10000 后直接跳过去，
 * 所以文件的第一个字节必须是 _start。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;                    /* 与 loader.asm 的 KERNEL_ADDR 一致 */

    /*
     * 内核自己的段。EXCLUDE_FILE 把用户程序 user.o 排除在外，它单独放进后面的 .user 段。
     * EXCLUDE_FILE 只作用于紧跟其后的一个段名，所以每个段名前都要写一次。
     */
    .text : {
        *(.text.entry)              /* entry.asm 的入口代码放在最前面 */
        *(EXCLUDE_FILE(*user.o) .text EXCLUDE_FILE(*user.o) .text.*)
    }

    .rodata : { *(EXCLUDE_FILE(*user.o) .rodata EXCLUDE_FILE(*user.o) .rodata.*) }
    .data   : { *(EXCLUDE_FILE(*user.o) .data EXCLUDE_FILE(*user.o) .data.*) }

    /*
     * 用户程序：首尾按 4 KB 对齐，paging.c 只给 __user_start～__user_end 之间的页打开用户位。
     * 代码和只读数据在前，__user_data 起是数据和用户栈，两部分之间也按 4 KB 对齐：
     * 前一部分的页对用户态只读，用户程序写自己的代码会触发缺页异常。
     * 其中的 .bss（用户栈）也写进 kernel.bin，以零的形式占用文件空间。
     */
    . = ALIGN(4096);
    .user : {
        __user_start = .;
        *user.o(.text .text.* .rodata .rodata.*)
        . = ALIGN(4096);
        __user_data = .;
        *user.o(.data .data.* .bss .bss.* COMMON)
        . = ALIGN(4096);
        __user_end = .;
    }

    /* .bss 不写进 kernel.bin，由 entry.asm 在运行时清零 */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}

ASSERT(_start == 0x10000, "_start must be at 0x10000, the address loader jumps to")
ASSERT(__user_data > __user_start, "user program must have code")
ASSERT(__user_end > __user_data, "user program must have a stack")
ASSERT(__user_end <= 0x400000, "paging.c only sets user bits in the first 4 MB page table")
