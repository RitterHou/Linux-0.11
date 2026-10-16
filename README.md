# 两阶段引导：从 A 到 ABP

这是 `learning` 分支上的独立实验。`main` 分支保留 Linux 0.11 原始源码。

## 运行

本机已安装 NASM 和 QEMU。在终端执行：

```sh
cd /Users/Ray/Projects/infra/linux-0.11
make run
```

QEMU 打开窗口后，左上角应显示 `ABP`：`A` 来自第一阶段，`B` 来自第二阶段在实模式下调用 BIOS 的输出，`P` 是第二阶段进入 32 位保护模式后直接写显存的输出。

关闭 QEMU 窗口，或在启动它的终端按 Ctrl+C，即可退出。修改代码后，先关闭旧窗口，再执行 `make run`。

- `make`：只生成镜像。
- `make clean`：删除生成的 `build/` 目录。
- 默认使用 macOS 的 Cocoa 窗口。其他系统可按 QEMU 支持情况设置，例如 `make run QEMU_DISPLAY=gtk`。
- Apple Silicon 本机是 ARM，QEMU 使用 TCG 模拟一台 x86 PC。

## 启动过程

1. BIOS 读取软盘的第一个扇区，加载到物理地址 `0x7C00`，检查 `55 AA` 签名后执行。
2. `boot.asm` 初始化段寄存器和栈，保存 BIOS 通过 `DL` 传入的启动盘编号，清屏并打印 `A`。
3. 第一阶段调用 `int 0x13` 的 `AH=0x02` 功能，从柱面 0、磁头 0、扇区 2 读取一个扇区到 `0000:8000`。
4. 若返回的进位标志 `CF=1`，打印 `E` 并停机，画面为 `AE`；此时不会执行第二阶段。
5. 若读取成功，执行远跳转 `jmp 0x0000:0x8000`，开始执行 `loader.asm`。
6. 第二阶段沿用栈和文本模式，不清屏，用 BIOS 接着打印 `B`。
7. `cli` 关闭中断，再通过 `0x92` 端口打开 A20 地址线。
8. `lgdt` 加载 GDT，置 CR0 的 PE 位，然后远跳转 `jmp 0x08:protected_mode`，用代码段选择子重新装载 CS，开始执行 32 位代码。
9. 把 DS、ES、FS、GS、SS 都设为数据段选择子 `0x10`，ESP 设为 `0x7C00`。
10. BIOS 在保护模式下不可用，直接向 VGA 文本显存 `0xB8004` 写入字符 `P` 和属性 `0x07`，然后通过 `hlt` 停机。

现在已经进入 32 位保护模式，但还没有 IDT（中断一直关闭）、没有加载内核，也没有建立分页。下一步：在实模式下先把内核读到 `0x10000`，再进入保护模式跳过去执行。

## 磁盘与内存布局

| 内容 | 磁盘镜像偏移 | 加载后的物理地址 | 谁负责加载 |
| --- | --- | --- | --- |
| 第一阶段 `boot.bin` | 0～511 | `0x7C00`～`0x7DFF` | BIOS |
| 第二阶段 `loader.bin`（含 GDT） | 512～1023 | `0x8000`～`0x81FF` | 第一阶段 |
| 其余空间 | 1024 起 | 不加载 | 无 |

栈从 `0x7C00` 向低地址增长，与上面两段代码不重叠。这里的 `0x8000` 是本实验选择的地址。VGA 文本显存从 `0xB8000` 开始，每个字符格占 2 字节：低字节是字符，高字节是颜色属性。第 0 行第 2 列的地址是 `0xB8000 + 2 × 2 = 0xB8004`。

CHS 的扇区号从 1 开始。镜像写入命令中的 `seek=1` 跳过一个 512 字节块，对应 CHS 的扇区 2。

磁盘镜像共 1,474,560 字节，没有文件系统。两个汇编文件各自生成恰好 512 字节的程序；只有第一阶段需要 `55 AA` 引导签名。

## GDT

| 选择子 | 描述符 | 含义 |
| --- | --- | --- |
| `0x00` | `0x0000000000000000` | 空描述符，CPU 规定第 0 项必须为空 |
| `0x08` | `0x00CF9A000000FFFF` | 代码段：基址 0，界限 4 GB，可执行可读，DPL 0，32 位 |
| `0x10` | `0x00CF92000000FFFF` | 数据段：基址 0，界限 4 GB，可读写，DPL 0 |

选择子的低 3 位是 RPL 和 TI，所以第 n 项的选择子是 n × 8。两个段都是平坦段，保护模式下的偏移就等于物理地址，写 `[0xb8004]` 访问的就是物理地址 `0xB8004`。

## 修改实验

- 改 `boot.asm` 中的 `mov al, 'A'`：改变第一阶段输出。
- 改 `loader.asm` 中的 `mov al, 'B'`：改变第二阶段在实模式下的输出。
- 改 `loader.asm` 中的 `'P'` 或属性 `0x07`：改变保护模式下的输出和颜色，例如 `0x1F` 是蓝底白字。
- 若要观察 BIOS 读盘失败，将 `boot.asm` 中的 `mov cl, 2` 暂时改成 `mov cl, 0`。CHS 扇区 0 无效，应看到 `AE`。实验后改回 2，关闭旧窗口并重新运行。
- 若要观察三重错误，将 `loader.asm` 中 `protected_mode` 后面的 `mov ax, DATA_SEL` 暂时改成 `mov ax, 0x18`。GDT 只有 3 项，`0x18` 超出界限，装入段寄存器时触发一般保护异常（13 号）；此时没有 IDT，处理异常再次出错变成双重错误（8 号），最终三重错误。Makefile 带有 `-no-reboot`，QEMU 窗口会直接关闭。用 `make run QEMU_FLAGS="-d int,cpu_reset -D qemu.log"` 运行，可在 `qemu.log` 中看到 `new 0xd`、`new 0x8` 和 `Triple fault`。实验后改回。

读取成功只表示扇区读取完成；这个最小实验尚未检查第二阶段的内容或校验和。

`bits 16`、`bits 32` 和 `org` 是汇编指令，不是 CPU 执行的机器指令。`org` 告诉汇编器加载地址，不会在文件开头填充相应数量的字节。`loader.asm` 中远跳转之前按 16 位编码，`protected_mode` 之后按 32 位编码，与 CPU 当时所处的模式一致。

## 文件

- `boot.asm`：第一阶段，带中文注释的读盘、错误处理和跳转代码。
- `loader.asm`：第二阶段，打印 `B` 后建立 GDT、打开 A20、进入保护模式，并直接写显存打印 `P`。
- `Makefile`：汇编、按扇区布局打包和运行 QEMU。
- `build/boot.bin`、`build/loader.bin`：两段机器码。
- `build/floppy.img`：用于启动的软盘镜像。

参考：[SeaBIOS 读盘实现](https://github.com/coreboot/seabios/blob/master/src/disk.c)、[NASM 引导扇区填充与签名](https://www.nasm.us/doc/nasm14.html)、[QEMU 启动参数](https://www.qemu.org/docs/master/system/qemu-manpage.html)、[OSDev：A20 地址线](https://wiki.osdev.org/A20_Line)、[OSDev：GDT](https://wiki.osdev.org/Global_Descriptor_Table)。
