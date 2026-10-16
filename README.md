# 两阶段引导：从 A 到 AB

这是 `learning` 分支上的独立实验。`main` 分支保留 Linux 0.11 原始源码。

## 运行

本机已安装 NASM 和 QEMU。在终端执行：

```sh
cd /Users/Ray/Projects/infra/linux-0.11
make run
```

QEMU 打开窗口后，左上角应显示 `AB`：`A` 来自第一阶段，`B` 来自从磁盘加载的第二阶段。

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
6. 第二阶段沿用栈和文本模式，不清屏，接着打印 `B`，然后通过 `hlt` 停机。

整个实验仍处于 16 位实模式，还没有进入保护模式、加载 Linux 内核或建立分页。下一步可让第二阶段建立 GDT 并进入 32 位保护模式。

## 磁盘与内存布局

| 内容 | 磁盘镜像偏移 | 加载后的物理地址 | 谁负责加载 |
| --- | --- | --- | --- |
| 第一阶段 `boot.bin` | 0～511 | `0x7C00`～`0x7DFF` | BIOS |
| 第二阶段 `loader.bin` | 512～1023 | `0x8000`～`0x81FF` | 第一阶段 |
| 其余空间 | 1024 起 | 不加载 | 无 |

栈从 `0x7C00` 向低地址增长，与上面两段代码不重叠。这里的 `0x8000` 是本实验选择的地址。

CHS 的扇区号从 1 开始。镜像写入命令中的 `seek=1` 跳过一个 512 字节块，对应 CHS 的扇区 2。

磁盘镜像共 1,474,560 字节，没有文件系统。两个汇编文件各自生成恰好 512 字节的程序；只有第一阶段需要 `55 AA` 引导签名。

## 修改实验

- 改 `boot.asm` 中的 `mov al, 'A'`：改变第一阶段输出。
- 改 `loader.asm` 中的 `mov al, 'B'`：改变第二阶段输出。
- 若要观察 BIOS 读盘失败，将 `boot.asm` 中的 `mov cl, 2` 暂时改成 `mov cl, 0`。CHS 扇区 0 无效，应看到 `AE`。实验后改回 2，关闭旧窗口并重新运行。

读取成功只表示扇区读取完成；这个最小实验尚未检查第二阶段的内容或校验和。

`bits 16` 和 `org` 是汇编指令，不是 CPU 执行的机器指令。`org` 告诉汇编器加载地址，不会在文件开头填充相应数量的字节。

## 文件

- `boot.asm`：第一阶段，带中文注释的读盘、错误处理和跳转代码。
- `loader.asm`：独立编译的第二阶段代码。
- `Makefile`：汇编、按扇区布局打包和运行 QEMU。
- `build/boot.bin`、`build/loader.bin`：两段机器码。
- `build/floppy.img`：用于启动的软盘镜像。

参考：[SeaBIOS 读盘实现](https://github.com/coreboot/seabios/blob/master/src/disk.c)、[NASM 引导扇区填充与签名](https://www.nasm.us/doc/nasm14.html)、[QEMU 启动参数](https://www.qemu.org/docs/master/system/qemu-manpage.html)。
