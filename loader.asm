; 第二阶段：由 boot.asm 从磁盘读到 0x8000，然后跳转到这里。
; 当前仍在 16 位实模式；沿用第一阶段设置的栈和文本显示模式。

bits 16
org 0x8000                      ; 必须与第一阶段的加载地址一致

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    cld

    mov ah, 0x0e                ; 不清屏，接着第一阶段的 A 输出 B
    mov al, 'B'
    mov bx, 0x0007              ; 显示页 0
    int 0x10

    cli
.halt:
    hlt                         ; 完成实验，保持 AB 画面
    jmp .halt

; 第一阶段只读取一个扇区，因此此处填充至 512 字节。
; 若代码超出一个扇区，NASM 会报错，避免生成未被完整加载的程序。
; 第二阶段由我们自己的代码加载，不需要 BIOS 的 55 AA 引导签名。
times 512 - ($ - $$) db 0
