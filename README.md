# 两阶段引导与 C 内核：从 A 到 kmain()

这是 `learning` 分支上的独立实验。`main` 分支保留 Linux 0.11 原始源码。

## 运行

需要 NASM、QEMU、系统自带的 clang，以及 Homebrew 的 lld（macOS 自带的 `ld` 不能链接 ELF）：

```sh
brew install nasm qemu lld
```

在终端执行：

```sh
cd /Users/Ray/Projects/infra/linux-0.11
make run
```

QEMU 打开窗口后，第 0 行应显示 `ABPKC`，第 2 行应显示 `Hello from kmain() in C!`：

- `A` 来自第一阶段。
- `B` 来自第二阶段在实模式下调用 BIOS 的输出。
- `P` 是第二阶段进入 32 位保护模式后直接写显存的输出。
- `K` 来自内核的汇编入口 `entry.asm`。
- `C` 和第 2 行来自 C 函数 `kmain()`。

关闭 QEMU 窗口，或在启动它的终端按 Ctrl+C，即可退出。修改代码后，先关闭旧窗口，再执行 `make run`。

- `make`：只生成镜像和 `build/kernel.elf`。
- `make clean`：删除生成的 `build/` 目录。
- 默认使用 macOS 的 Cocoa 窗口。其他系统可按 QEMU 支持情况设置，例如 `make run QEMU_DISPLAY=gtk`。
- Apple Silicon 本机是 ARM，QEMU 使用 TCG 模拟一台 x86 PC。

## 启动过程

1. BIOS 读取软盘的第一个扇区，加载到物理地址 `0x7C00`，检查 `55 AA` 签名后执行。
2. `boot.asm` 初始化段寄存器和栈，保存 BIOS 通过 `DL` 传入的启动盘编号，清屏并打印 `A`。
3. 第一阶段调用 `int 0x13` 的 `AH=0x02` 功能，从柱面 0、磁头 0、扇区 2 读取一个扇区到 `0000:8000`。
4. 若返回的进位标志 `CF=1`，打印 `E` 并停机，画面为 `AE`；此时不会执行第二阶段。
5. 若读取成功，执行远跳转 `jmp 0x0000:0x8000`，开始执行 `loader.asm`。
6. 第二阶段保存 `DL` 中的启动盘编号，沿用栈和文本模式，不清屏，用 BIOS 接着打印 `B`。
7. 第二阶段再调用 `int 0x13`，从扇区 3 起一次读取 16 个扇区（`KERNEL_SECTORS`）到 `1000:0000`，即物理地址 `0x10000`。失败时先复位磁盘系统再重试，3 次都失败则打印 `E` 并停机，画面为 `ABE`。
8. `cli` 关闭中断，再通过 `0x92` 端口打开 A20 地址线。
9. `lgdt` 加载 GDT，置 CR0 的 PE 位，然后远跳转 `jmp 0x08:protected_mode`，用代码段选择子重新装载 CS，开始执行 32 位代码。
10. 把 DS、ES、FS、GS、SS 都设为数据段选择子 `0x10`，ESP 设为 `0x7C00`。
11. BIOS 在保护模式下不可用，直接向 VGA 文本显存 `0xB8004` 写入字符 `P` 和属性 `0x07`。
12. 执行 `jmp eax` 跳到 `0x10000`，即内核入口 `_start`（`entry.asm`）。
13. `_start` 把 ESP 换成内核自己的 4 KB 栈，把 `.bss` 清零，向 `0xB8006` 写入 `K`，然后 `call kmain`。
14. `kmain()`（`kernel.c`）从第 0 行第 4 列写入 `C`，再换两行打印 `Hello from kmain() in C!`，最后循环执行 `hlt`。

内核必须在第 7 步、也就是仍处于实模式时读入：进入保护模式后 BIOS 的 `int 0x13` 就不能再用了，而我们还没有自己的磁盘驱动。Linux 0.11 也是这样做的：`bootsect.s` 在实模式下把内核读到 `0x10000`（`SYSSEG = 0x1000`），`head.s` 设好栈后再跳进 C 的 `main()`。

现在内核已经能用 C 编写，但还没有 IDT（中断一直关闭），也没有建立分页。下一步：建立 IDT，重新设置 8259 中断控制器，打开定时器和键盘中断。

## 内核的构建过程

```text
entry.asm ──nasm -f elf32──▶ entry.o ─┐
                                      ├─ ld.lld + linker.ld ─▶ kernel.bin（写进软盘）
kernel.c  ──clang -c───────▶ kernel.o ─┘                    └─▶ kernel.elf（带符号，供调试）
```

- `clang --target=i386-elf` 让系统自带的 clang 生成 32 位 x86 的 ELF 目标文件，而不是 macOS 程序。其他编译选项的作用写在 `Makefile` 的注释里，核心是 `-ffreestanding`：没有标准库，不能用 `printf`，也没有 `main` 入口。
- `linker.ld` 把链接地址定为 `0x10000`，并把 `entry.asm` 的 `.text.entry` 段排在最前面。loader 不解析任何文件格式，只是把 `kernel.bin` 原样读到 `0x10000` 后跳过去，所以文件的第一个字节必须是 `_start`。脚本末尾的 `ASSERT` 在这一点不成立时让链接失败。
- `ld.lld --oformat=binary` 输出去掉 ELF 头的纯机器码，作为 `kernel.bin`。`.bss` 在最后，不占文件空间。
- `kernel.elf` 不进软盘，可以用系统自带的 `objdump -d build/kernel.elf` 反汇编，或用 `objdump -t build/kernel.elf` 查看各符号的地址。

一次构建中各段的地址如下（具体数值随代码变化，以 `objdump -h build/kernel.elf` 为准）：

| 段 | 地址 | 内容 |
| --- | --- | --- |
| `.text` | `0x10000` 起 | `_start` 在最前，后面是 `kmain` 等 C 函数 |
| `.rodata` | 紧随 `.text` | 字符串 `"Hello from kmain() in C!"` |
| `.bss` | 紧随其后，约 4 KB | 内核栈和 `cursor_row`、`cursor_col` |

## 为什么要清零 .bss

C 语言规定，没有初值的全局变量和 `static` 变量初值为 0。编译器把它们放进 `.bss` 段，`kernel.bin` 里不存它们的内容，以节省空间。在普通程序里，由操作系统在加载时清零；我们的内核没有人帮忙，只能在 `_start` 中自己用 `rep stosb` 清零。

`kernel.c` 中的光标位置 `cursor_row`、`cursor_col` 就在 `.bss` 里。去掉清零后，在 QEMU 中运行通常仍然正常，因为 QEMU 启动时内存全是 0；只有当这块内存里原本有别的内容时才会出错。下面的修改实验演示了这种情况。

## 磁盘与内存布局

| 内容 | 磁盘镜像偏移 | 加载后的物理地址 | 谁负责加载 |
| --- | --- | --- | --- |
| 第一阶段 `boot.bin` | 0～511 | `0x7C00`～`0x7DFF` | BIOS |
| 第二阶段 `loader.bin`（含 GDT） | 512～1023 | `0x8000`～`0x81FF` | 第一阶段 |
| 内核 `kernel.bin`（读 16 个扇区） | 1024～9215 | `0x10000`～`0x11FFF` | 第二阶段 |
| 其余空间 | 9216 起 | 不加载 | 无 |

loader 的栈从 `0x7C00` 向低地址增长；进入内核后改用 `.bss` 中的内核栈。这里的 `0x8000` 是本实验选择的地址。VGA 文本显存从 `0xB8000` 开始，每个字符格占 2 字节：低字节是字符，高字节是颜色属性。第 r 行第 c 列的地址是 `0xB8000 + (r × 80 + c) × 2`。

CHS 的扇区号从 1 开始。镜像写入命令中的 `seek=1` 跳过一个 512 字节块，对应 CHS 的扇区 2；`seek=2` 对应扇区 3。

1.44 MB 软盘每条磁道有 18 个扇区。第二阶段只在柱面 0、磁头 0 这条磁道上一次读完，所以内核最多能用扇区 3～18，共 16 个扇区（8 KB）。读取目标 `0x10000`～`0x11FFF` 不跨越 64 KB 边界，满足软驱 DMA 的要求。

磁盘镜像共 1,474,560 字节，没有文件系统。`boot.bin` 和 `loader.bin` 各自恰好 512 字节；只有第一阶段需要 `55 AA` 引导签名。`kernel.bin` 按实际代码长度生成，内容不足 16 个扇区时，第二阶段多读到的部分是镜像中的零。

## 内核扇区数

`KERNEL_SECTORS` 只在 `Makefile` 中定义一次（当前为 16），有两处用到它：

- 汇编 `loader.asm` 时通过 `-DKERNEL_SECTORS=16` 传入，决定读多少个扇区。直接运行 `nasm loader.asm` 而不传这个值，会报错终止。
- 打包镜像前检查 `kernel.bin` 的大小，超过 `KERNEL_SECTORS × 512` 字节时 `make` 报错，避免生成一个只读入半个内核的镜像。

若把 `KERNEL_SECTORS` 改成大于 16，汇编 `loader.asm` 会报错，因为当前的一次性读取无法跨越磁道。`.bss` 不计入 `kernel.bin` 的大小，但它在内存中紧跟内核之后，不能覆盖其他正在使用的内存。

## GDT

| 选择子 | 描述符 | 含义 |
| --- | --- | --- |
| `0x00` | `0x0000000000000000` | 空描述符，CPU 规定第 0 项必须为空 |
| `0x08` | `0x00CF9A000000FFFF` | 代码段：基址 0，界限 4 GB，可执行可读，DPL 0，32 位 |
| `0x10` | `0x00CF92000000FFFF` | 数据段：基址 0，界限 4 GB，可读写，DPL 0 |

选择子的低 3 位是 RPL 和 TI，所以第 n 项的选择子是 n × 8。两个段都是平坦段，保护模式下的偏移就等于物理地址，写 `[0xb8004]` 访问的就是物理地址 `0xB8004`，`jmp eax` 跳到的也就是物理地址 `0x10000`。C 代码中的指针值同样直接就是物理地址。

## 修改实验

- 改 `boot.asm` 中的 `mov al, 'A'`：改变第一阶段输出。
- 改 `loader.asm` 中的 `mov al, 'B'`：改变第二阶段在实模式下的输出。
- 改 `loader.asm` 中的 `'P'` 或属性 `0x07`：改变保护模式下的输出和颜色，例如 `0x1F` 是蓝底白字。
- 改 `entry.asm` 中的 `'K'`：改变内核入口的输出。
- 改 `kernel.c` 中 `kmain()` 打印的字符串，或 `VGA_GRAY`：改变 C 代码的输出。
- 若要观察 BIOS 读盘失败，将 `boot.asm` 中的 `mov cl, 2` 暂时改成 `mov cl, 0`。CHS 扇区 0 无效，应看到 `AE`。实验后改回 2，关闭旧窗口并重新运行。
- 若要观察读内核失败，将 `loader.asm` 中的 `mov cl, KERNEL_START_SECTOR` 暂时改成 `mov cl, 0`。重试 3 次后应看到 `ABE`。实验后改回。
- 若要观察三重错误，将 `loader.asm` 中 `protected_mode` 后面的 `mov ax, DATA_SEL` 暂时改成 `mov ax, 0x18`。GDT 只有 3 项，`0x18` 超出界限，装入段寄存器时触发一般保护异常（13 号）；此时没有 IDT，处理异常再次出错变成双重错误（8 号），最终三重错误。Makefile 带有 `-no-reboot`，QEMU 窗口会直接关闭。用 `make run QEMU_FLAGS="-d int,cpu_reset -D qemu.log"` 运行，可在 `qemu.log` 中看到 `new 0xd`、`new 0x8` 和 `Triple fault`。实验后改回。
- 若要观察不清零 `.bss` 的后果，将 `entry.asm` 中的 `rep stosb` 暂时注释掉，执行 `make`，再用下面的命令在镜像中内核之后的位置填入 `0x01`，然后 `make run`：

  ```sh
  head -c 6000 /dev/zero | tr '\0' '\1' | dd of=build/floppy.img bs=1 seek=1536 conv=notrunc
  ```

  loader 会把这些字节一起读进 `.bss` 所在的内存。`cursor_row` 不再是 0，`C` 和第 2 行都不会出现在屏幕上，只剩 `ABPK`。恢复 `rep stosb` 并再次填入同样的字节，输出恢复正常。命令从镜像偏移 1536（内核文件的第 512 字节）开始填写，适用于 `kernel.bin` 小于 512 字节的情况。实验后执行 `make clean`，再重新 `make run`。

读取成功只表示扇区读取完成；这个最小实验尚未检查第二阶段和内核的内容或校验和。

`bits 16`、`bits 32` 和 `org` 是汇编指令，不是 CPU 执行的机器指令。`org` 告诉汇编器加载地址，不会在文件开头填充相应数量的字节。`loader.asm` 中远跳转之前按 16 位编码，`protected_mode` 之后按 32 位编码，与 CPU 当时所处的模式一致。`entry.asm` 编译成 ELF 目标文件，加载地址由链接脚本决定，所以不写 `org`。

## 文件

- `boot.asm`：第一阶段，带中文注释的读盘、错误处理和跳转代码。
- `loader.asm`：第二阶段，打印 `B`，在实模式下读入内核，再建立 GDT、打开 A20、进入保护模式，写显存打印 `P` 后跳进内核。
- `entry.asm`：内核的汇编入口 `_start`，设置内核栈、清零 `.bss`、打印 `K` 后调用 `kmain()`。
- `kernel.c`：内核的 C 部分，直接写 VGA 显存输出字符和字符串。
- `linker.ld`：内核链接脚本，规定链接地址 `0x10000` 和各段顺序。
- `Makefile`：汇编、编译、链接、检查内核大小、按扇区布局打包和运行 QEMU。
- `build/boot.bin`、`build/loader.bin`、`build/kernel.bin`：写进软盘的三段机器码。
- `build/kernel.elf`：带符号的内核，供反汇编和调试使用。
- `build/floppy.img`：用于启动的软盘镜像。

参考：[SeaBIOS 读盘实现](https://github.com/coreboot/seabios/blob/master/src/disk.c)、[NASM 引导扇区填充与签名](https://www.nasm.us/doc/nasm14.html)、[QEMU 启动参数](https://www.qemu.org/docs/master/system/qemu-manpage.html)、[OSDev：A20 地址线](https://wiki.osdev.org/A20_Line)、[OSDev：GDT](https://wiki.osdev.org/Global_Descriptor_Table)、[LLD 链接脚本](https://lld.llvm.org/ELF/linker_script.html)。
