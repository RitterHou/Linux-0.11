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

; 目前只有内核态，段寄存器始终是同一组平坦段，所以这里不保存 DS 等段寄存器。
; 以后有了用户态，就要在这里保存并切换成内核数据段。
isr_common:
    pushad                      ; 保存 8 个通用寄存器
    cld                         ; C 代码要求 DF=0
    push esp                    ; 参数：指向栈上的 struct interrupt_frame
    call interrupt_dispatch
    add esp, 4
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
