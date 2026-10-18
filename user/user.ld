/*
 * 用户程序的链接脚本：从用户空间的底部 0x40000000 开始放。
 * 代码和只读数据在前；数据从下一个 4 KB 边界开始，这样 exec 能把代码页映射成只读、数据页映射成可写。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    __end = .;                      /* 程序的末尾：exec 把堆的起点（brk）设在这里 */
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}
