; 内核：由 loader.asm 在实模式下读到 0x10000，进入保护模式后跳到这里。
; 段寄存器和栈已由 loader 设好（平坦段，ESP=0x7C00），这里直接写显存打印 K。
; 成功时画面为 ABPK。

bits 32
org 0x10000                     ; 必须与 loader 读入内核的地址一致

start:
    ; 第 0 行第 3 列 = 0xB8000 + 3 × 2，紧接在 ABP 后面。
    mov byte [0xb8000 + 3 * 2], 'K'
    mov byte [0xb8000 + 3 * 2 + 1], 0x07    ; 黑底浅灰字，与前面相同

.halt:
    hlt                         ; 完成实验，保持 ABPK 画面
    jmp .halt
