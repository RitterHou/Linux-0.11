; 内核入口：由 loader.asm 在实模式下读到 0x10000，进入保护模式后跳到这里。
; 链接脚本把本段放在内核最前面，所以 _start 正好位于 0x10000。
; 这里换上内核自己的栈、清零 .bss、打印 K，然后调用 C 函数 kmain()。

bits 32

global _start
extern kmain
extern __bss_start, __bss_end   ; 由 linker.ld 定义

section .text.entry progbits alloc exec nowrite align=16
_start:
    mov esp, stack_top          ; 不再借用 loader 的 0x7C00，换成内核自己的栈

    ; C 语言约定：未初始化的全局变量和 static 变量初值为 0。
    ; 它们位于 .bss，不占 kernel.bin 的空间，也就不会从磁盘读入，必须在这里清零。
    cld                         ; rep stosb 向高地址填充；C 代码也要求 DF=0
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi                ; .bss 的字节数
    xor eax, eax
    rep stosb

    ; 第 0 行第 3 列 = 0xB8000 + 3 × 2，紧接在 ABP 后面。
    mov byte [0xb8000 + 3 * 2], 'K'
    mov byte [0xb8000 + 3 * 2 + 1], 0x07    ; 黑底浅灰字，与前面相同

    call kmain

    ; kmain 不应返回；万一返回，就停在这里。
    cli
.halt:
    hlt
    jmp .halt

section .bss
align 16
stack_bottom:
    resb 4096                   ; 4 KB 内核栈，向低地址增长
stack_top:
