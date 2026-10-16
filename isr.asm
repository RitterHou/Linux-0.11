; 中断入口：IDT 的前 48 项分别指向这里的 isr0～isr47。
; 0～31 是 CPU 异常，32～47 是重新映射后的 8259 IRQ0～IRQ15。
; 每个入口把栈整理成统一的 struct interrupt_frame（见 kernel.h），
; 再交给 C 函数 interrupt_dispatch() 处理。

bits 32

extern interrupt_dispatch
global isr_stub_table

; CPU 只对这几个异常自动压入错误码，其余入口补一个 0，让栈布局保持一致。
%define HAS_ERROR_CODE(n) ((n) == 8 || ((n) >= 10 && (n) <= 14) || (n) == 17 || (n) == 21 || (n) == 29 || (n) == 30)

section .text

%assign n 0
%rep 48
isr%+n:
%if !HAS_ERROR_CODE(n)
    push dword 0                ; 没有错误码，补一个 0
%endif
    push dword n                ; 中断号
    jmp isr_common
%assign n n + 1
%endrep

KERNEL_DATA_SEL equ 0x10        ; 与 kernel.h 中的定义一致

; 中断可能打断任何代码，被打断时 DS 等段寄存器不一定是内核数据段
; （以后的用户态程序就会用自己的段）。CPU 只自动切换 CS 和 SS，
; 所以这里先保存 DS、ES、FS、GS，换成内核数据段再进入 C 代码，返回前原样恢复。
isr_common:
    pushad                      ; 保存 8 个通用寄存器
    push ds
    push es
    push fs
    push gs
    mov ax, KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld                         ; C 代码要求 DF=0
    push esp                    ; 参数：指向栈上的 struct interrupt_frame
    call interrupt_dispatch
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8                  ; 丢掉中断号和错误码
    iretd                       ; 弹出 EIP、CS、EFLAGS，回到被打断的地方

; 48 个入口的地址表，interrupt.c 用它填写 IDT。
section .rodata
align 4
isr_stub_table:
%assign n 0
%rep 48
    dd isr%+n
%assign n n + 1
%endrep
