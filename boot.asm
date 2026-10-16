; 第一阶段：BIOS 加载本扇区，打印 A，再加载并启动第二阶段。
; 成功后由第二阶段和内核接着输出，第 0 行最终为 ABPKC；读盘失败显示 AE，并停机。

bits 16
org 0x7c00                      ; BIOS 加载第一个扇区的物理地址

jmp 0x0000:start                ; 统一 CS:IP 为 0000:start

start:
    cli                         ; 设置栈时暂时关闭可屏蔽中断
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00              ; 栈向低地址增长，不覆盖两阶段代码
    cld
    mov [boot_drive], dl        ; 保存 BIOS 传入的启动盘编号
    sti

    mov ax, 0x0003              ; 设置 80×25 文本模式并清屏
    int 0x10

    mov ah, 0x0e                ; 输出字符 A
    mov al, 'A'
    mov bx, 0x0007              ; 显示页 0
    int 0x10

    ; int 13h / AH=02h：按柱面、磁头、扇区号（CHS）读盘。
    ; 这里从启动盘的柱面 0、磁头 0、扇区 2 读取一个扇区。
    ; CHS 扇区号从 1 开始，所以扇区 2 对应镜像文件偏移 512。
    xor ax, ax
    mov es, ax
    mov bx, 0x8000              ; 目标 ES:BX = 0000:8000
    mov ah, 0x02                ; BIOS 功能：读取扇区
    mov al, 1                   ; 读取 1 个扇区，即 512 字节
    mov ch, 0                   ; 柱面 0
    mov cl, 2                   ; 扇区 2
    mov dh, 0                   ; 磁头 0
    mov dl, [boot_drive]        ; 使用实际启动盘，不写死盘号
    int 0x13
    jc disk_error              ; CF=1 表示失败；此时不能跳到第二阶段

    mov dl, [boot_drive]        ; 把启动盘编号继续传给第二阶段
    jmp 0x0000:0x8000           ; 将执行权交给刚加载的代码

disk_error:
    mov ah, 0x0e
    mov al, 'E'                 ; 读盘失败时，画面为 AE
    mov bx, 0x0007
    int 0x10
    cli
.halt:
    hlt
    jmp .halt

boot_drive db 0

times 510 - ($ - $$) db 0        ; 第一阶段必须放进一个引导扇区
dw 0xaa55                       ; 小端存储为 55 AA，供 BIOS 检查
