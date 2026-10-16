/*
 * 内核链接脚本：决定各段在内存中的地址和顺序。
 * loader 把 kernel.bin 原样读到 0x10000 后直接跳过去，
 * 所以文件的第一个字节必须是 _start。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;                    /* 与 loader.asm 的 KERNEL_ADDR 一致 */

    .text : {
        *(.text.entry)              /* entry.asm 的入口代码放在最前面 */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }   /* 字符串常量等只读数据 */
    .data   : { *(.data .data.*) }       /* 有初值的全局变量 */

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
