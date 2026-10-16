; 第二阶段：由 boot.asm 从磁盘读到 0x8000，然后跳转到这里。
; 先在实模式下用 BIOS 打印 B，并趁 BIOS 还能用把内核逐个扇区读到 0x10000；
; 再建立 GDT、打开 A20、进入 32 位保护模式，直接写显存打印 P，最后跳进内核。
; 成功后由内核接着输出，第 0 行最终为 ABPKCVIT；读内核失败时画面为 ABE，并停机。

bits 16
org 0x8000                      ; 必须与第一阶段的加载地址一致

CODE_SEL equ gdt_code - gdt_start   ; 代码段选择子 0x08
DATA_SEL equ gdt_data - gdt_start   ; 数据段选择子 0x10

KERNEL_SEG          equ 0x1000  ; 内核读到 1000:0000，即物理地址 0x10000
KERNEL_ADDR         equ KERNEL_SEG * 16
KERNEL_START_LBA    equ 2       ; 从 0 起编号的扇区号（LBA）：0 是 boot，1 是 loader，内核从 2 开始
%define KERNEL_MAX_SECTORS 512  ; 最多 256 KB，即 0x10000～0x4FFFF

; 内核占多少个扇区由 Makefile 按 kernel.bin 的实际大小算出，并通过 -D 传进来。
%ifndef KERNEL_SECTORS
%fatal "KERNEL_SECTORS 未定义：请用 make 构建，它会传入 -DKERNEL_SECTORS"
%elif KERNEL_SECTORS > KERNEL_MAX_SECTORS
%fatal "内核超过 256 KB，会读进 0x50000 以上，与本实验的内存安排冲突"
%endif

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    cld
    mov [boot_drive], dl        ; 第一阶段通过 DL 传来启动盘编号

    mov ah, 0x0e                ; 不清屏，接着第一阶段的 A 输出 B
    mov al, 'B'
    mov bx, 0x0007              ; 显示页 0
    int 0x10

    ; 读内核：进入保护模式后就不能再调用 BIOS 读盘，所以必须现在读。
    ; 先问 BIOS 软盘的几何参数（int 0x13 AH=08h）：CL 低 6 位是每条磁道的扇区数，
    ; DH 是最大磁头号。它还会把 ES:DI 改成软盘参数表的地址，所以要保存 ES。
    push es
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    pop es
    jc disk_error
    and cl, 0x3f
    mov [sectors_per_track], cl
    inc dh
    mov [heads], dh

    ; 逐个扇区读入：每读一个，目标段地址加 512 / 16 = 0x20。
    ; 每次只读 512 字节，起点又按 512 对齐，永远不会跨越 64 KB 边界，满足软驱 DMA 的限制。
read_sector:
    mov byte [read_tries], 3
.retry:
    ; LBA 换算成 CHS：
    ;   扇区 = LBA % 每道扇区数 + 1（CHS 的扇区号从 1 开始）
    ;   磁头 = (LBA / 每道扇区数) % 磁头数
    ;   柱面 = (LBA / 每道扇区数) / 磁头数
    mov ax, [lba]
    xor dx, dx
    movzx bx, byte [sectors_per_track]
    div bx                      ; AX = LBA / 每道扇区数，DX = 余数
    mov cl, dl
    inc cl                      ; CL = 扇区号
    xor dx, dx
    movzx bx, byte [heads]
    div bx                      ; AX = 柱面，DX = 磁头
    mov ch, al                  ; CH = 柱面（软盘只有 80 个柱面，8 位够用）
    mov dh, dl                  ; DH = 磁头
    mov dl, [boot_drive]
    mov es, [load_seg]
    xor bx, bx                  ; ES:BX = 目标地址
    mov ax, 0x0201              ; AH=02h 读取扇区，AL=1 个
    int 0x13
    jnc .next

    ; 真实软驱常因马达还没转稳而读失败，先复位磁盘系统再重试，每个扇区最多 3 次。
    dec byte [read_tries]
    jz disk_error
    xor ah, ah                  ; BIOS 功能：复位磁盘系统
    mov dl, [boot_drive]
    int 0x13
    jmp .retry

.next:
    add word [load_seg], 512 / 16
    inc word [lba]
    dec word [sectors_left]
    jnz read_sector
    jmp kernel_loaded

disk_error:
    mov ah, 0x0e
    mov al, 'E'                 ; 读内核失败时，画面为 ABE
    mov bx, 0x0007
    int 0x10
    cli
.halt:
    hlt
    jmp .halt

kernel_loaded:
    xor ax, ax
    mov es, ax

    ; 保护模式下还没有 IDT，也不能再调用 BIOS，来一个中断就会三重错误。
    cli

    ; 打开 A20：用 0x92 端口的“快速 A20”，否则第 20 位地址线恒为 0，
    ; 访问 1 MB 以上会回绕。bit 0 写 1 会复位机器，必须保持为 0。
    in al, 0x92
    or al, 0x02
    and al, 0xfe
    out 0x92, al

    lgdt [gdt_descriptor]       ; 把 GDT 的界限和基址装入 GDTR

    mov eax, cr0
    or eax, 1                   ; 置 PE 位：从这条指令之后进入保护模式
    mov cr0, eax

    ; 远跳转：用代码段选择子重新装载 CS，同时清掉按实模式预取的指令。
    jmp CODE_SEL:protected_mode

bits 32
protected_mode:
    mov ax, DATA_SEL            ; 其余段寄存器都换成数据段选择子
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7c00             ; 沿用原来的栈顶

    ; BIOS 已不可用，直接写 VGA 文本显存。每格 2 字节：字符、属性。
    ; 第 0 行第 2 列 = 0xB8000 + 2 × 2，正好在 AB 后面。
    mov byte [0xb8000 + 2 * 2], 'P'
    mov byte [0xb8000 + 2 * 2 + 1], 0x07    ; 黑底浅灰字，与 A、B 相同

    ; 跳进内核。段是平坦的，0x10000 既是偏移也是物理地址。
    mov eax, KERNEL_ADDR
    jmp eax

boot_drive        db 0
read_tries        db 3
sectors_per_track db 0
heads             db 0
lba               dw KERNEL_START_LBA     ; 下一个要读的扇区
sectors_left      dw KERNEL_SECTORS       ; 还要读几个扇区
load_seg          dw KERNEL_SEG           ; 下一个扇区放到的段地址

; GDT 每项 8 字节。代码段和数据段都是基址 0、界限 4 GB 的“平坦”段，
; 所以保护模式下的偏移就等于物理地址。
align 8
gdt_start:
    dq 0                        ; 第 0 项必须是空描述符
gdt_code:
    dq 0x00cf9a000000ffff       ; 代码段：可执行可读，DPL 0，32 位，4 KB 粒度
gdt_data:
    dq 0x00cf92000000ffff       ; 数据段：可读写，DPL 0，32 位，4 KB 粒度
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; GDT 界限：字节数减 1
    dd gdt_start                ; GDT 基址：DS 为 0，偏移就是线性地址

; 第一阶段只读取一个扇区，因此此处填充至 512 字节。
; 若代码超出一个扇区，NASM 会报错，避免生成未被完整加载的程序。
; 第二阶段由我们自己的代码加载，不需要 BIOS 的 55 AA 引导签名。
times 512 - ($ - $$) db 0
