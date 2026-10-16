# 两阶段引导与 C 内核

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

QEMU 打开窗口后应看到：

| 行 | 内容 | 来源 |
| --- | --- | --- |
| 0 | `ABPKCVITU` | `A` 第一阶段；`B` 第二阶段在实模式下调用 BIOS；`P` 第二阶段进入保护模式后写显存；`K` 内核汇编入口 `entry.asm`；`C` C 函数 `kmain()`；`V` 打开分页后通过虚拟地址 `0xC0000000` 写入；`I` 打开中断之后；`T` 开始任务调度之后；`U` 内核第一次收到来自用户态的系统调用 |
| 2 | `Hello from kmain() in C!` | `kmain()` |
| 4 | `ticks: N` | 定时器中断，每秒加 100 |
| 5 | `paging: on, 0xC0000000 -> 0xB8000` | 分页已打开 |
| 6 | `tasks: AAAAAAAAAABBBBBBBBBB…` | 任务 A（内核态）和任务 B（用户态）轮流运行，都通过 `write` 系统调用写自己的字母，每段约 10 个；写满一行后从行首覆盖 |
| 7 | 平时为空 | 任务结束时的黄色消息，例如 `task 2 killed by #PF page fault: …` 或 `task 2 exited with code 7` |
| 8～18 | `keyboard: ` 和输入的内容 | 键盘回显窗口：在 QEMU 窗口里打字会回显在这里，写满后整个窗口向上滚动一行，窗口外的内容不受影响 |
| 19～23 | 平时为空 | 出现 CPU 异常时，红底白字的异常面板，见下文“异常” |

键盘支持字母、数字、常用符号、空格、回车、退格、左右 Shift 和 Caps Lock（美式布局），暂不支持方向键、Ctrl、Alt 和小键盘。打字前先点一下 QEMU 窗口，让它获得键盘焦点。

关闭 QEMU 窗口，或在启动它的终端按 Ctrl+C，即可退出。修改代码后，先关闭旧窗口，再执行 `make run`。

- `make`：只生成镜像和 `build/kernel.elf`。
- `make test`：在无头 QEMU 中运行全部自动化测试，见下文“自动化测试”。
- `make clean`：删除生成的 `build/` 目录。
- 默认使用 macOS 的 Cocoa 窗口。其他系统可按 QEMU 支持情况设置，例如 `make run QEMU_DISPLAY=gtk`。
- Apple Silicon 本机是 ARM，QEMU 使用 TCG 模拟一台 x86 PC。

## 自动化测试

```sh
make test
```

`tests/test_kernel.py` 只用 Python 标准库。它在后台启动没有窗口的 QEMU，通过 QEMU 监视器读取显存和寄存器、模拟按键，再用系统自带的 `objdump` 查符号地址和反汇编。目前检查：

- 启动后各行的输出；内存中内核的 `.text` 和 `.rodata` 与 `kernel.bin` 一致（`.data` 运行时会被改写，不比较）；处于保护模式，各段寄存器的值正确，GDTR 指向内核自己的 `gdt`。
- 分页已打开，CR3 指向 `page_directory`；用监视器的 `gva2gpa` 逐个检查虚拟地址：`0xC0000000` 映射到 `0xB8000`，第 0 页和 16 MB 以上未映射，其余一一对应。
- 定时器频率约为 100 Hz（允许 90～110）。
- 直接喂扫描码（自检 `KEYBOARD_EDGE`）：按住 Caps Lock 产生的重复按下码只切换一次；输入超出一行后，退格能跨过行首回到上一行行尾。
- 初始化 8259 之前就有一次按键时，键盘之后仍能正常回显（自检 `EARLY_KEY` 在 `pic_init()` 之前等到键盘控制器里有一个字节，故意不读走）。
- 模拟按键后的回显、回车和退格，退格不会删到上一行；左右 Shift 得到大写字母和 `!@_` 等符号，Caps Lock 只影响字母，与 Shift 同时生效时得到小写。
- 连续输入 13 行后，回显窗口第 8～18 行按顺序向上滚动，第 0～6 行和第 19～24 行不受影响。
- 任务等待时 CPU 停在 `sleep` 系统调用（`sys_sleep`）的 `hlt` 上，且 IF 为 1。
- GDT 第 6、7 项是 DPL 3 的用户代码段和数据段；只有用户程序所在的页打开了用户位，内核的代码、数据、IDT、TSS 和显存所在的页都没有。
- 任务 B 运行在用户态：它的内核栈顶是从特权级 3 进入系统调用时 CPU 压入的现场（CS 为 `0x33`、SS 为 `0x3B`、返回地址在 `user_main` 内、ESP 在用户栈内、IOPL 为 0），TSS 中的 `ss0:esp0` 指向这个内核栈。
- 用户程序写内核变量或执行 `cli` 时，只有任务 B 被结束：第 7 行给出 `#PF`（错误码 7：页存在、写操作、来自用户态）或 `#GP` 以及出错指令的 EIP，不出现异常面板，定时器照常走，第 6 行很快只剩 A，TR 停在任务 A；用户程序调用 `exit(7)` 时同样只剩任务 A，第 7 行为 `task 2 exited with code 7`；把内核地址交给 `write` 得到 -1。
- 任务 B 在用户态空转时注入 NMI（自检 `USER_SPIN`，先用监视器的 `stop` 暂停、确认 CS 为 `0x33` 再注入）：显示异常面板而不是结束任务 B，面板上的 SS、ESP 是用户态的值。
- 任务 A、B 都结束后切回任务 0：定时器照常走，TR 为 `0x18`，CPU 停在 `kmain()` 的 `hlt` 上，CR3 仍是页目录。
- IDT 第 `0x80` 项是指向 `isr_syscall` 的 DPL 3 陷阱门；`getpid`、`write`、`sleep` 和未知调用号的返回值正确，`write` 的内容出现在第 6 行。
- 两个任务轮流运行：第 6 行只有 A 和 B、多次交替、每段不超过 15 个字母，0.5 秒后内容有变化；TR 在任务 A、B 的 TSS 选择子 `0x20` 和 `0x28` 之间切换。
- 寄存器设为已知值后执行 `ud2`：异常面板显示 `#UD`，EAX～EBP、EIP（`ud2` 的地址）、各段寄存器、CR3 都与预期一致，ESP 落在内核栈内，EFLAGS 的 IF 为 1。
- 除以零显示 `#DE`，EIP 正好指向 `div` 指令；触发未填写的 IDT 表项显示 `#GP`，错误码为 `0x182`。
- 读未映射的 `0xD0000000` 显示 `#PF`，错误码 0，CR2 为该地址；写空指针显示 `#PF`，错误码 2（写操作），CR2 为 0。
- 把 loader 所在的 `0x8000`～`0x81FF` 清零后，定时器照常工作：内核已不再依赖 loader 的 GDT。
- 内核被撑到约 90 个扇区、跨越 3 个柱面时，loader 也能完整读入：末尾的标记能读到，`0x10000` 起的内存与 `kernel.bin` 一致。
- 把 DS、ES、FS、GS 换成一个基址为 1 MB 的数据段后空转，定时器照常工作，空闲时这些段寄存器仍是原来的值：中断入口会换成内核段，返回前再恢复。这里不用空选择子测试，因为 QEMU 的 TCG 模拟不检查通过空选择子访问内存，换不换段都看不出区别。

后面几项需要故意制造异常或破坏内存。测试脚本会用 `make BUILD=build/selftest/<名称> SELFTEST=<名称>` 另外构建自检内核：`SELFTEST` 非空时，`selftest.c` 中对应的代码会被编译进去，在打开中断后触发异常。正常构建不定义 `SELFTEST`，`selftest_run()` 是空函数。

全部测试约需 12 秒（自检内核已构建过时）。任何一项失败时脚本以非 0 状态退出，并打印期望值与实际值。

## 启动过程

1. BIOS 读取软盘的第一个扇区，加载到物理地址 `0x7C00`，检查 `55 AA` 签名后执行。
2. `boot.asm` 初始化段寄存器和栈，保存 BIOS 通过 `DL` 传入的启动盘编号，清屏并打印 `A`。
3. 第一阶段调用 `int 0x13` 的 `AH=0x02` 功能，从柱面 0、磁头 0、扇区 2 读取一个扇区到 `0000:8000`。
4. 若返回的进位标志 `CF=1`，先复位磁盘系统再重试；3 次都失败则打印 `E` 并停机，画面为 `AE`，此时不会执行第二阶段。真实软驱刚上电或刚换盘时，第一次读常常失败。
5. 若读取成功，执行远跳转 `jmp 0x0000:0x8000`，开始执行 `loader.asm`。
6. 第二阶段保存 `DL` 中的启动盘编号，沿用栈和文本模式，不清屏，用 BIOS 接着打印 `B`。
7. 第二阶段用 `int 0x13` 的 `AH=0x08` 功能查询软盘的几何参数，再从 LBA 2（CHS 扇区 3）起逐个扇区读取 `KERNEL_SECTORS` 个扇区到 `0x10000` 起的内存，读取时把 LBA 换算成柱面、磁头、扇区号，可以跨越磁道和柱面。某个扇区读失败时先复位磁盘系统再重试，3 次都失败则打印 `E` 并停机，画面为 `ABE`。
8. `cli` 关闭中断，再通过 `0x92` 端口打开 A20 地址线。
9. `lgdt` 加载 GDT，置 CR0 的 PE 位，然后远跳转 `jmp 0x08:protected_mode`，用代码段选择子重新装载 CS，开始执行 32 位代码。
10. 把 DS、ES、FS、GS、SS 都设为数据段选择子 `0x10`，ESP 设为 `0x7C00`。
11. BIOS 在保护模式下不可用，直接向 VGA 文本显存 `0xB8004` 写入字符 `P` 和属性 `0x07`。
12. 执行 `jmp eax` 跳到 `0x10000`，即内核入口 `_start`（`entry.asm`）。
13. `_start` 把 ESP 换成内核自己的 4 KB 栈，把 `.bss` 清零，向 `0xB8006` 写入 `K`，然后 `call kmain`。
14. `kmain()`（`kernel.c`）从第 0 行第 4 列写入 `C`，再换两行打印 `Hello from kmain() in C!`。
15. `kmain()` 调用 `gdt_init()` 换上内核自己的 GDT，调用 `idt_init()` 装好 IDT，再调用 `paging_init()` 建立页表并打开分页，然后通过虚拟地址 `0xC0000000` 在第 0 行写入 `V`，在第 5 行写出分页信息。
16. `kmain()` 调用 `pic_init()`、`keyboard_init()`、`timer_init(100)`、`sched_init()`，重新设置 8259、读光键盘控制器里残留的字节、把定时器设为每秒 100 次、建好任务 A（内核态）和任务 B（用户态）的 TSS，然后写出第 4、6、8 行的标签。
17. `sti` 打开中断，在第 0 行写入 `I`，调用 `selftest_run()`（正常构建中为空）。
18. `sched_start()` 开始任务调度，在第 0 行写入 `T`。下一个定时器中断就切换到任务 A，此后每 20 个 tick 在 A、B 之间切换一次。第一次切到任务 B 时 CPU 直接进入特权级 3；B 第一次执行系统调用时，内核在第 0 行写入 `U`。任务等待时停在 `hlt`，每来一个中断就醒来处理一次。

内核必须在第 7 步、也就是仍处于实模式时读入：进入保护模式后 BIOS 的 `int 0x13` 就不能再用了，而我们还没有自己的磁盘驱动。Linux 0.11 也是这样做的：`bootsect.s` 在实模式下把内核读到 `0x10000`（`SYSSEG = 0x1000`），`head.s` 设好栈后再跳进 C 的 `main()`。

现在内核已经能响应定时器、键盘中断和 CPU 异常，打开了分页；一个内核态任务和一个用户态任务在定时器的驱动下轮流运行，用户态任务只能通过 `int 0x80` 系统调用请内核做事。

## 内核的构建过程

```text
内核的 .asm 文件 ──nasm -f elf32──▶ *.o ─┐
                                        ├─ ld.lld + linker.ld ─┬─▶ kernel.bin（写进软盘）
内核的 .c 文件 ────clang -c────────▶ *.o ─┘                      └─▶ kernel.elf（带符号，供调试）
```

- `clang --target=i386-elf` 让系统自带的 clang 生成 32 位 x86 的 ELF 目标文件，而不是 macOS 程序。其他编译选项的作用写在 `Makefile` 的注释里，核心是 `-ffreestanding`：没有标准库，不能用 `printf`，也没有 `main` 入口。
- `linker.ld` 把链接地址定为 `0x10000`，并把 `entry.asm` 的 `.text.entry` 段排在最前面。loader 不解析任何文件格式，只是把 `kernel.bin` 原样读到 `0x10000` 后跳过去，所以文件的第一个字节必须是 `_start`。脚本末尾的 `ASSERT` 在这一点不成立时让链接失败。
- `ld.lld --oformat=binary` 输出去掉 ELF 头的纯机器码，作为 `kernel.bin`。`.bss` 在最后，不占文件空间。
- `kernel.elf` 不进软盘，可以用系统自带的 `objdump -d build/kernel.elf` 反汇编，或用 `objdump -t build/kernel.elf` 查看各符号的地址。
- 修改 `kernel.h`、`io.h` 或 `Makefile` 后，所有 C 文件都会重新编译。

一次构建中各段的地址如下（具体数值随代码变化，以 `objdump -h build/kernel.elf` 为准）：

| 段 | 地址 | 内容 |
| --- | --- | --- |
| `.text` | `0x10000` 起，约 2.3 KB | `_start` 在最前，后面是 48 个中断入口和各个 C 函数 |
| `.rodata` | 紧随 `.text` | 中断入口地址表、异常名称、扫描码表、字符串 |
| `.data` | 紧随 `.rodata` | 有非 0 初值的全局变量，例如回显窗口的最后一行 `window_bottom` |
| `.user` | 下一个 4 KB 边界起，2 页 | 用户程序 `user.c` 的代码、数据和 4 KB 用户栈，只有这两页打开用户位 |
| `.bss` | `.user` 之后，约 41 KB | 4 KB 内核栈、2 KB 的 IDT、页目录和 5 张页表（共 24 KB）、3 个 TSS、任务 A 和 B 各 4 KB 的栈、光标位置、`timer_ticks` 等。页目录和页表要求 4 KB 对齐，`.bss` 从 4 KB 边界开始 |

## 为什么要清零 .bss

C 语言规定，没有初值的全局变量和 `static` 变量初值为 0。编译器把它们放进 `.bss` 段，`kernel.bin` 里不存它们的内容，以节省空间。在普通程序里，由操作系统在加载时清零；我们的内核没有人帮忙，只能在 `_start` 中自己用 `rep stosb` 清零。

现在依赖这一点的有 `timer_ticks`（ticks 从 0 开始数）、`keyboard.c` 中的 `line_length`、IDT 中没有填写的 48～255 号表项（全 0 才是确定的“非法门”），以及页目录中没有用到的 1019 项：它们的存在位必须是 0，否则 CPU 会把垃圾当成页表地址，访问那些虚拟地址时读写到任意物理内存。

在 QEMU 中去掉清零，目前看不出差别：SeaBIOS 开机自检时会把常规内存清零，开机前写进内存的值到内核运行时都已变成 0；loader 只读入 `kernel.bin` 本身占用的扇区，不会读到 `.bss` 所在的内存。第 3 步（提交 `a716a38`）的 README 中有一个能看到效果的实验：当时光标变量没有被显式赋值，又位于读入范围内，往镜像里写入垃圾后输出就乱了。在真实硬件上，或由别的引导程序加载时，内存不一定是 0，所以清零不能省。

## 中断

### 中断的处理路径

```text
定时器 / 键盘 ──IRQ──▶ 8259 ──中断号 0x20 / 0x21──┐
                                                ├─▶ CPU 查 IDT ─▶ isr.asm 的 isrN ─▶ interrupt_dispatch()
CPU 异常（除零、#GP 等）──中断号 0～31─────────┘
```

1. CPU 收到中断号 N，在 IDT 中取出第 N 项，跳到对应的入口 `isrN`（`isr.asm`）。中断门会自动清除 IF，处理期间不会再被别的中断打断。CPU 自动压入 EFLAGS、CS、EIP，少数异常还会压入错误码。
2. `isrN` 对没有错误码的中断补压一个 0，再压入中断号 N，然后跳到公共部分 `isr_common`。这样所有中断的栈布局都相同。
3. `isr_common` 用 `pushad` 保存通用寄存器，再保存 DS、ES、FS、GS，并把它们换成内核数据段 `0x10`：中断可能打断任何代码，那时的段寄存器不一定是内核的，而 CPU 只自动切换 CS（以及特权级变化时的 SS）。然后把栈顶地址作为参数调用 `interrupt_dispatch()`。栈上的内容就是 `kernel.h` 中的 `struct interrupt_frame`。
4. `interrupt_dispatch()`（`interrupt.c`）按中断号分发：0～31 号交给 `exception_panic()`，0x20 交给 `timer_handle()`，0x21 交给 `keyboard_handle()`。外设中断先向 8259 发送 EOI 再处理，原因见下文“任务切换”。
5. 回到 `isr_common`，恢复 GS、FS、ES、DS，`popad` 恢复通用寄存器，丢掉中断号和错误码，`iretd` 回到被打断的地方，通常是 `kmain()` 的 `hlt` 循环。

### IDT

IDT 有 256 项，每项 8 字节，放在 `.bss` 中。`idt_init()` 把前 48 项填成 32 位中断门，入口地址来自 `isr.asm` 中的地址表，选择子是内核 GDT 的代码段 `0x08`；第 `0x80` 项是系统调用的入口，见下文“系统调用”；其余各项保持全 0。最后用 `lidt` 把 IDT 的界限和基址装入 IDTR。

全 0 的表项类型字段为 0，不是合法的门描述符。若被触发，CPU 在检查存在位之前就产生 13 号一般保护异常，错误码为 `中断号 × 8 + 2`（第 1 位表示错误来自 IDT）。

### 8259 中断控制器

两片 8259 级联，管理 16 根 IRQ 线：主片负责 IRQ0～7，从片负责 IRQ8～15，从片接在主片的 IRQ2 上。BIOS 把主片映射到中断号 `0x08`～`0x0F`，与 CPU 异常 8～15 重叠：不改的话，每次定时器中断都会被当成 8 号双重错误。`pic_init()` 重新发送 4 个初始化命令字，把 IRQ0～15 映射到 `0x20`～`0x2F`，并只打开 IRQ0（定时器）和 IRQ1（键盘）。Linux 0.11 的 `setup.s` 做的是同样的事。

8259 在送出一个中断后，要等 CPU 回复 EOI（End Of Interrupt），才会送来同级和更低优先级的中断。忘记发 EOI，定时器只会响一次。

### 定时器

8253 定时器的输入时钟是 1.193180 MHz。`timer_init(hz)` 把分频值 `1193180 / hz` 写进通道 0，于是每秒产生 `hz` 次 IRQ0。分频值是 16 位的，所以 `hz` 至少为 19。`timer_handle()` 把 `timer_ticks` 加 1，并刷新第 4 行的数字。Linux 0.11 的 `sched_init()` 用同样的方法设置 100 Hz 的时钟，用来驱动进程调度。

### 键盘

每按下或松开一个键，键盘控制器就把一个扫描码放进 `0x60` 端口，并发出 IRQ1。`keyboard_handle()` 必须读走扫描码，控制器才会送来下一个。扫描码最高位为 1 表示松开。`keyboard.c` 记录左右 Shift 是否按住（要同时处理按下和松开）、Caps Lock 是否打开（每按下一次切换；按住不放时键盘会重复发送按下码，这些重复的按下码不再切换），其余键只处理按下：查普通表或 Shift 表转成字符，交给 `vga_putc()` 回显，同时移动屏幕上闪烁的硬件光标。`kmain()` 用 `vga_set_window(8, 18)` 把 `vga_putc()` 限制在第 8～18 行：光标写过第 18 行时，窗口内的行整体上移一行、最后一行清空，第 0～6 行的固定内容和第 19 行以下都不受影响。Caps Lock 只影响字母，与 Shift 同时生效时字母为小写，与真实键盘一致。方向键等扩展键先送一个前缀 `0xE0` 再送一个字节，目前连同后一个字节一起忽略。键盘上的 Caps Lock 指示灯需要向键盘发命令才会亮，这里没有做。

`keyboard_init()` 在 `pic_init()` 之后、打开中断之前，把键盘控制器输出缓冲区里残留的字节读光。若开中断前就有按键（开机时按住某个键，或 BIOS 菜单留下的松键码），这个字节没人读，键盘控制器就让 IRQ1 一直保持高电平；8259 是边沿触发，初始化命令又重置了它的边沿检测，之后再也等不到上升沿，键盘中断就永远不来了。

### 异常

0～31 号是 CPU 异常，目前都无法恢复。`exception_panic()` 在第 19～23 行画一块红底白字的面板，显示异常号、名称和出错那一刻的全部寄存器，然后停机。面板在键盘回显窗口下面，滚动时不会被移走。例如自检 `REGISTER_DUMP` 的面板：

```text
Exception 6 #UD invalid opcode
EIP=0x00011126 CS=0x0008 EFLAGS=0x00010202 error=0x00000000
EAX=0x11111111 EBX=0x22222222 ECX=0x33333333 EDX=0x44444444
ESI=0x55555555 EDI=0x66666666 EBP=0x77777777 ESP=0x00012FF0
DS=0x0010 ES=0x0010 FS=0x0010 GS=0x0010 CR2=0x00000000 CR3=0x00018000
```

- 寄存器的值都取自栈上的 `struct interrupt_frame`，也就是中断入口保存下来的、异常发生那一刻的状态。
- 同一特权级内发生异常时，CPU 不压入 ESP，所以出错时的 ESP 就是栈上 EFLAGS 之后的地址。
- CR2 只在缺页异常时有意义，是出错的虚拟地址。CR3 是页目录的物理地址。
- EFLAGS 中的 `0x10000` 是 RF 位，CPU 在这类故障异常时自动置位。
- 用 `objdump -d build/kernel.elf` 按 EIP 可以找到出错的那条指令。

## 系统调用

程序执行 `int 0x80` 请求内核服务，约定与 Linux 0.11 相同：EAX 是调用号，EBX、ECX、EDX 依次是第 1～3 个参数，返回值放回 EAX。`syscall.h` 定义调用号和 `write()`、`getpid()`、`sleep()` 等包装函数，相当于 Linux 0.11 `include/unistd.h` 中的 `_syscall3` 等宏；内核一侧在 `syscall.c`。

| 调用号 | 函数 | 作用 | 返回值 |
| --- | --- | --- | --- |
| 0 | `write(fd, buf, count)` | 目前只支持 `fd` 为 1：把 `count` 个字符写到第 6 行的任务输出区 | 写出的字节数；`fd` 不对、或用户态传来的 `buf` 不在用户程序的页内时为 -1 |
| 1 | `getpid()` | 取当前任务号 | 0、1 或 2 |
| 2 | `sleep(ticks)` | 等待若干个定时器 tick，期间 CPU 停在 `hlt` | 0 |
| 3 | `exit(code)` | 结束当前任务，在第 7 行写出结束代码 | 不返回 |
| 其他 | | | -1 |

调用的过程：

```text
write(1, &c, 1)                       syscall.h：EAX=0，EBX=1，ECX=&c，EDX=1，执行 int $0x80
  └▶ CPU 查 IDT 第 0x80 项           DPL 3 的陷阱门，跳到 isr_syscall
      └▶ isr_common                  与其他中断相同：保存寄存器，换成内核数据段
          └▶ interrupt_dispatch()    中断号 0x80，交给 syscall_dispatch()；不是外设中断，不发 EOI
              └▶ syscall_dispatch()  按 EAX 查 syscall_table，调用 sys_write(EBX, ECX, EDX)
                  返回值写进栈上保存的 EAX
      ◀── isr_common 用 popad 恢复寄存器，EAX 里就是返回值；iretd 回到 int 的下一条指令
```

- 门的 DPL 决定谁可以用 `int n` 调用它：`int n` 要求当前特权级在数值上不大于门的 DPL。中断门和异常入口都是 DPL 0，用户态程序（特权级 3）执行 `int 0x20` 之类会触发 `#GP`；只有第 `0x80` 项是 DPL 3。Linux 0.11 用 `set_system_gate(0x80, &system_call)` 做同样的设置。
- 第 `0x80` 项用陷阱门而不是中断门：陷阱门进入时不清 IF，系统调用执行期间定时器照常中断，`sleep` 才能等到 tick 增加，也可以在等待时切换到别的任务。
- 栈上保存的 EAX 就是 `struct interrupt_frame` 中的 `eax`。`syscall_dispatch()` 改写它，中断返回时 `popad` 把它装回 EAX，这是把返回值交给调用方的方法。

## 用户态

x86 有 4 个特权级，这里只用 0（内核态）和 3（用户态）。当前特权级（CPL）就是 CS 的低 2 位。任务 A 在特权级 0 运行内核里的 `task_main()`；任务 B 在特权级 3 运行 `user.c` 中的 `user_main()`，它不能执行 `cli`、`hlt`、`in`、`out` 等特权指令，不能访问内核的内存和显存，要做任何事都只能通过 `int 0x80` 请内核帮忙。

| 机制 | 做法 | Linux 0.11 的对应做法 |
| --- | --- | --- |
| 用户段 | GDT 第 6、7 项是 DPL 3 的代码段和数据段，选择子 `0x33`、`0x3B` 的 RPL 也是 3 | 每个进程在 LDT 中有自己的用户段，选择子 `0x0f`、`0x17` |
| 进入用户态 | 任务 B 的 TSS 中 CS、DS 等都是用户段选择子；硬件任务切换装入这个 TSS 时，CPU 直接进入特权级 3 | `move_to_user_mode()` 在栈上构造一个返回现场，用 `iret` 落到特权级 3 |
| 内核栈 | TSS 中的 `ss0:esp0` 指向任务 B 的 4 KB 内核栈 | `copy_process()` 把 `esp0` 设为该进程内核栈的顶端 |
| 内存保护 | 页表项的用户位：只有 `.user` 段的两页可以从特权级 3 访问 | 用户页的页表项是 7（存在、可写、用户） |
| 检查用户指针 | 来自用户态的 `write`，`buf` 必须完全落在用户程序的页内 | `verify_area()` |

用户态发生中断或系统调用时，CPU 要先换栈：用户栈不可信，内核不能在上面工作。

```text
用户态（CPL 3）执行 int $0x80
  1. CPU 从当前 TSS 取出 ss0:esp0，把 SS、ESP 换成任务 B 的内核栈
  2. 在内核栈上依次压入用户态的 SS、ESP、EFLAGS、CS、EIP
  3. 从 IDT 第 0x80 项装入 CS=0x08，CPL 变成 0，跳到 isr_syscall
  4. isr_common 保存寄存器，把 DS 等从用户段 0x3B 换成内核段 0x10
     ……处理系统调用……
  5. iretd 弹出 EIP、CS、EFLAGS，发现 CS 的 RPL 是 3，再弹出 ESP、SS，回到用户态
```

`kernel.h` 中的 `struct interrupt_frame` 末尾因此多了 `user_esp`、`user_ss` 两项，只有从用户态进入时才有效；异常面板据此显示出错时真正的 ESP 和 SS。

页目录项和页表项都有用户位，CPU 取两者的“与”：前 16 MB 的页目录项都打开了用户位，再由页表项逐页决定，内核的页在页表项中不开用户位。用户程序的代码、数据和栈由链接脚本集中放进 `.user` 段，首尾按 4 KB 对齐，`paging_init()` 只给这几页打开用户位。`linker.ld` 用 `EXCLUDE_FILE(*user.o)` 把 `user.o` 排除在内核的 `.text`、`.rodata`、`.data` 之外；`EXCLUDE_FILE` 只作用于紧跟其后的一个段名，所以每个段名前都要写一次。

内核能读写所有内存，所以系统调用不能盲目相信用户传来的指针。若不检查，用户程序把内核地址交给 `write`，内核就会替它把内核内存印出来。`sys_write()` 对来自用户态的调用用 `user_range_ok()` 检查 `buf`～`buf + count` 是否完全落在用户程序的页内，不在就返回 -1。

### 用户态出错只结束出错的任务

内核自己出错时无法恢复，只能显示异常面板并停机；但用户程序出错是用户程序的事，不应该拖垮整个系统。`interrupt_dispatch()` 收到 0～31 号异常时先看栈上保存的 CS：

- 低 2 位是 3，说明出错时在用户态。`user_exception()` 在第 7 行用黄字写出 `task 2 killed by #PF page fault: EIP=… error=…`，然后调用 `sched_exit_current()` 结束这个任务。
- 否则是内核自己出错，照旧显示异常面板并停机。
- 例外是 NMI（2 号）、双重错误（8 号）和机器检查（18 号）：它们来自硬件，或者表示内核处理异常时又出了错，与用户程序无关，即使发生时 CPU 恰好在用户态，也照样显示异常面板。这时面板上的 CS、SS、ESP 是用户态的值。

`sched_exit_current()` 把当前任务标记为已结束，再切到下一个还在运行的任务，不再返回；被结束的任务不会再被调度，它的 TSS 和栈就此闲置。`exit(code)` 系统调用走的是同一条路，只是第 7 行写 `task 2 exited with code 7`。Linux 0.11 的 `kernel/traps.c` 中，各异常处理函数调用 `die()`：打印寄存器后调用 `do_exit(11)` 结束当前进程（11 是 `SIGSEGV` 的编号）；`do_exit()` 还要释放内存页、关闭文件、通知父进程，最后同样调用 `schedule()` 切走。

## 任务切换

`sched.c` 用 x86 的硬件任务切换让任务 A、B 轮流运行，与 Linux 0.11 的做法相同。

- 每个任务有一个 TSS（任务状态段，104 字节），里面存着这个任务的全部寄存器：EIP、ESP、EFLAGS、通用寄存器、段寄存器和 CR3。切换时 CPU 保存前几类寄存器，但 CR3 只从新 TSS 装入、不保存回旧 TSS，所以每个 TSS 的 CR3 都要预先填好，包括任务 0 的。GDT 第 3、4、5 项是任务 0、A、B 的 TSS 描述符，选择子分别为 `0x18`、`0x20`、`0x28`。
- 任务 0 就是 `kmain()` 自己。`sched_init()` 用 `ltr 0x18` 把它登记为当前任务，CPU 第一次切换时才知道把当前寄存器存到哪里。任务 A、B 的 TSS 预先填好入口地址、各自 4 KB 栈的栈顶、`EFLAGS = 0x202`（IF=1）、内核段选择子和当前的 CR3。
- `switch_to(n)` 执行一条远跳转 `ljmp` 到任务 n 的 TSS 选择子。CPU 把当前寄存器存进当前任务的 TSS，再从目标 TSS 装入全部寄存器，于是目标任务从它上次被切走的地方接着运行；第一次运行的任务从入口开始。Linux 0.11 的 `switch_to` 宏也是一条 `ljmp`。
- 定时器中断每次调用 `schedule()`，时间片（20 个 tick）用完就按 1 → 2 → 1 … 的顺序切到下一个还在运行的任务，与 Linux 0.11 的 `do_timer()` 调用 `schedule()` 相同。下一个任务就是自己时不切换：远跳转到自己的 TSS 会因为它处于“忙”状态而触发 `#GP`。
- 任务 0 在第一次切换后不再被调度，除非任务 A、B 都已结束：那时切回任务 0，它从上次被切走的定时器中断里返回，停在 `kmain()` 的 `hlt` 循环中当空闲任务。测试 `ALL_EXIT` 让任务 A、B 都调用 `exit`，检查此时 TR 回到 `0x18`、CR3 仍是页目录、定时器照常工作。Linux 0.11 的任务 0 也是在没有别的进程可运行时才被调度。
- 任务 A 运行内核里的 `task_main()`，任务 B 运行用户程序 `user.c` 里的 `user_main()`，两者做同样的事：用 `getpid()` 得到任务号，任务 1 写 `A`、任务 2 写 `B`，每写一个字母 `sleep(2)`，所以每段约 10 个。它们只通过系统调用输出和等待，见上文“系统调用”和下文“用户态”。
- 两个任务共用输出位置。`task_write_char()` 写之前关中断、写完恢复原来的中断状态，保证“读位置、写字符、位置加一”不被切换打断。

两个顺序上的要求：

- **先发 EOI 再处理中断。** 切换发生在定时器处理函数里。任务 A 第一次运行时从 `task_a()` 的开头开始，不会回到那个处理函数；若 EOI 留到处理完才发，8259 就再也不会送来定时器中断，屏幕停在一个 `A` 上。中断门已清除 IF，提前发 EOI 也不会导致中断嵌套。Linux 0.11 的 `timer_interrupt` 同样一开始就发 EOI。
- **自检代码在开始调度之前运行。** 有的自检要让 `kmain()` 一直停在原地，开始调度后它就被切走、不再回来了。

每次硬件任务切换都会置位 CR0 的 TS 位，此后第一次使用 x87/SSE 指令会触发 7 号异常。内核编译时用了 `-mgeneral-regs-only`，不会生成这类指令。

## 分页

`paging.c` 中的 `paging_init()` 建立两级页表，装入 CR3，再打开 CR0 的 PG 位。此后 CPU 访问的每个地址都是虚拟地址，要查表才能得到物理地址：

```text
虚拟地址 0xC0000004 = 1100000000 0000000000 000000000100
                       └ 高 10 位 ┘ └ 中 10 位 ┘ └ 低 12 位 ┘
                         768          0            4
CR3 → 页目录第 768 项 → alias_table → 第 0 项 → 物理页 0xB8000 → + 4 = 0xB8004
```

| 虚拟地址 | 物理地址 | 说明 |
| --- | --- | --- |
| `0x00000000`～`0x00000FFF` | 未映射 | 第 0 页，用来捕获空指针 |
| `0x00001000`～`0x00FFFFFF` | 与虚拟地址相同 | 前 16 MB 一一对应，4 张页表；其中只有 `.user` 段的两页打开用户位 |
| `0xC0000000`～`0xC0000FFF` | `0xB8000`～`0xB8FFF` | 显存的第二个地址，1 张页表 |
| 其余 | 未映射 | 访问会触发 14 号缺页异常 |

- 打开分页的那一刻，下一条指令的地址已经要经过页表翻译。前 16 MB 一一对应，所以代码、栈、显存和 GDT 的地址都不变，内核照常运行。Linux 0.11 的 `head.s` 同样一一对应地映射前 16 MB，不过它把页目录放在物理地址 0，我们放在 `.bss` 里。
- 页目录项和页表项的低 12 位是标志，这里只用到存在位（1）和可写位（2）。存在位为 0 时访问该地址，CPU 产生 14 号缺页异常，把出错的虚拟地址放进 CR2，错误码的第 0 位表示页是否存在、第 1 位表示是不是写操作。异常面板的最后一行显示 CR2。
- 第 0 页不映射后，`*(int *)0 = 1` 这样的空指针写入会立即触发缺页，而不是悄悄改写物理地址 0。
- 写 `0xC0000000` 和写 `0xB8000` 改的是同一块显存。第 0 行的 `V` 就是 `kmain()` 通过 `0xC0000000` 写进去的。

## 磁盘与内存布局

| 内容 | 磁盘镜像偏移 | 加载后的物理地址 | 谁负责加载 |
| --- | --- | --- | --- |
| 第一阶段 `boot.bin` | 0～511 | `0x7C00`～`0x7DFF` | BIOS |
| 第二阶段 `loader.bin`（含 GDT） | 512～1023 | `0x8000`～`0x81FF` | 第一阶段 |
| 内核 `kernel.bin`（`KERNEL_SECTORS` 个扇区） | 1024 起 | `0x10000` 起 | 第二阶段 |
| 其余空间 | 内核之后 | 不加载 | 无 |

loader 的栈从 `0x7C00` 向低地址增长；进入内核后改用 `.bss` 中的内核栈。这里的 `0x8000` 是本实验选择的地址。VGA 文本显存从 `0xB8000` 开始，每个字符格占 2 字节：低字节是字符，高字节是颜色属性。第 r 行第 c 列的地址是 `0xB8000 + (r × 80 + c) × 2`。

进入内核后，loader 所在的 `0x7C00`～`0x81FF` 就不再需要了：内核用 `gdt_init()` 换上了自己的 GDT，栈也换成了 `.bss` 中的内核栈。

CHS 的扇区号从 1 开始。镜像写入命令中的 `seek=1` 跳过一个 512 字节块，对应 CHS 的扇区 2；`seek=2` 对应扇区 3。

1.44 MB 软盘有 80 个柱面、2 个磁头，每条磁道 18 个扇区。BIOS 按 CHS（柱面、磁头、扇区）读盘，第二阶段把从 0 开始连续编号的 LBA 换算成 CHS：

```text
扇区 = LBA % 每道扇区数 + 1      （CHS 的扇区号从 1 开始）
磁头 = (LBA / 每道扇区数) % 磁头数
柱面 = (LBA / 每道扇区数) / 磁头数
```

例如 LBA 20 是柱面 0、磁头 1、扇区 3，LBA 36 是柱面 1、磁头 0、扇区 1。每道扇区数和磁头数来自 `AH=0x08` 的查询结果，而不是写死的常数；查询结果中每道扇区数为 0 时改用 18，磁头数用 16 位保存（最大磁头号 255 加 1 是 256），保证后面的除法不会除以零。读完内核后，loader 向端口 `0x3F2` 写 0 关掉软驱马达：接下来要关中断、重设 8253 和 8259，BIOS 原本靠定时器中断关马达的那段逻辑不会再运行。Linux 0.11 的 `bootsect.s` 用 `kill_motor` 做同样的事。每次只读一个 512 字节的扇区，目标地址又按 512 对齐，所以永远不会跨越 64 KB 边界，满足软驱 DMA 的要求。Linux 0.11 的 `bootsect.s` 一次读一整条磁道以加快速度，原理相同。

磁盘镜像共 1,474,560 字节，没有文件系统。`boot.bin` 和 `loader.bin` 各自恰好 512 字节；只有第一阶段需要 `55 AA` 引导签名。`kernel.bin` 按实际代码长度生成，最后一个扇区不满 512 字节时，第二阶段多读到的部分是镜像中的零。

## 内核扇区数

loader 要读多少个扇区，由 `Makefile` 在汇编 `loader.asm` 时按 `kernel.bin` 的实际大小算出：`KERNEL_SECTORS = (字节数 + 511) / 512`，通过 `-DKERNEL_SECTORS=…` 传入。所以 `loader.bin` 依赖 `kernel.bin`，内核一变，loader 也会重新汇编。直接运行 `nasm loader.asm` 而不传这个值，会报错终止。

内核最多 512 个扇区（256 KB，读到 `0x10000`～`0x4FFFF`），超过时汇编 `loader.asm` 会报错。`.bss` 不计入 `kernel.bin` 的大小，但它在内存中紧跟内核之后，不能覆盖其他正在使用的内存。

## GDT

loader 进入保护模式时用的是写在 `loader.bin` 里的 GDT。进入内核后，`gdt.c` 中的 `gdt_init()` 在 `.bss` 里建一份内容相同的新表，用 `lgdt` 装入，再用一次远跳转重新装载 CS、用 `mov` 重新装载其余段寄存器：段寄存器缓存着装入时读到的描述符，换表后不重新装载就仍在用旧描述符。选择子保持不变，所以 IDT 等处引用的 `0x08`、`0x10` 都不用改。新表有 8 项，第 3～7 项留给以后的 TSS 等描述符，用 `gdt_set_entry()` 填写。Linux 0.11 的 `head.s` 也会重建 GDT，因为 `setup.s` 留下的那份在 `0x90000` 附近，之后会被覆盖。

| 选择子 | 描述符 | 含义 |
| --- | --- | --- |
| `0x00` | `0x0000000000000000` | 空描述符，CPU 规定第 0 项必须为空 |
| `0x08` | `0x00CF9A000000FFFF` | 代码段：基址 0，界限 4 GB，可执行可读，DPL 0，32 位 |
| `0x10` | `0x00CF92000000FFFF` | 数据段：基址 0，界限 4 GB，可读写，DPL 0 |
| `0x18`、`0x20`、`0x28` | 由 `sched_init()` 填写 | 任务 0、A、B 的 TSS：基址为对应 TSS 的地址，界限 103，类型为 32 位可用 TSS |
| `0x33` | `0x00CFFA000000FFFF` | 用户代码段（第 6 项）：与内核代码段相同，只是 DPL 为 3；选择子的 RPL 也是 3 |
| `0x3B` | `0x00CFF2000000FFFF` | 用户数据段（第 7 项），DPL 3 |
| `0x48` | 仅在自检内核中填写 | 第 9 项：自检 `OTHER_SEGMENTS` 用的基址 1 MB 的数据段 |

选择子的低 3 位是 RPL 和 TI，所以第 n 项的选择子是 n × 8。两个段都是平坦段，保护模式下的偏移就等于物理地址，写 `[0xb8004]` 访问的就是物理地址 `0xB8004`，`jmp eax` 跳到的也就是物理地址 `0x10000`。C 代码中的指针值同样直接就是物理地址。

## 修改实验

- 改 `boot.asm` 中的 `mov al, 'A'`：改变第一阶段输出。
- 改 `loader.asm` 中的 `mov al, 'B'`：改变第二阶段在实模式下的输出。
- 改 `loader.asm` 中的 `'P'` 或属性 `0x07`：改变保护模式下的输出和颜色，例如 `0x1F` 是蓝底白字。
- 改 `entry.asm` 中的 `'K'`：改变内核入口的输出。
- 改 `kernel.c` 中 `kmain()` 打印的字符串，或 `kernel.h` 中的 `VGA_GRAY`：改变 C 代码的输出。
- 若要观察 BIOS 读盘失败，将 `boot.asm` 中的 `mov cl, 2` 暂时改成 `mov cl, 0`。CHS 扇区 0 无效，重试 3 次后应看到 `AE`。实验后改回 2，关闭旧窗口并重新运行。
- 若要观察读内核失败，将 `loader.asm` 中 `read_sector` 里紧接着 `mov dh, dl` 的 `mov dl, [boot_drive]` 暂时改成 `mov dl, 0x81`（一块不存在的硬盘）。每次读都会失败，重试 3 次后应看到 `ABE`。实验后改回。
- 若要观察三重错误，将 `loader.asm` 中 `protected_mode` 后面的 `mov ax, DATA_SEL` 暂时改成 `mov ax, 0x18`。GDT 只有 3 项，`0x18` 超出界限，装入段寄存器时触发一般保护异常（13 号）；此时还没有 IDT，处理异常再次出错变成双重错误（8 号），最终三重错误。Makefile 带有 `-no-reboot`，QEMU 窗口会直接关闭。用 `make run QEMU_FLAGS="-d int,cpu_reset -D qemu.log"` 运行，可在 `qemu.log` 中看到 `new 0xd`、`new 0x8` 和 `Triple fault`。实验后改回。
- 若要观察忘记发 EOI 的后果，将 `interrupt.c` 中 `interrupt_dispatch()` 里的 `pic_send_eoi(vector);` 暂时注释掉。`ticks` 停在 1，键盘也没有反应：8259 一直等着 CPU 回复，不再送来定时器和优先级更低的键盘中断。实验后改回。
- 若要改变回显窗口的位置和大小，修改 `kernel.c` 中的 `CONSOLE_TOP` 和 `CONSOLE_BOTTOM`。窗口不要覆盖第 0～6 行，否则滚动时会把这些固定内容一起移走。
- 若要改变任务切换的快慢，修改 `sched.c` 中的 `TASK_SLICE`。改成 4，每段只有约 2 个字母；改成 60，每段约 30 个。
- 若要观察 EOI 发晚了的后果，把 `interrupt_dispatch()` 中的 `pic_send_eoi(vector);` 挪到处理函数之后。切到任务 A 后定时器就停了，第 6 行只有一个 `A`，键盘也没有反应。
- 若要改变时钟频率，修改 `kernel.c` 中的 `TIMER_HZ`。改成 50，`ticks` 每秒加 50。改成 1000 也能明显变快，但在 QEMU 的 TCG 模拟下会丢失部分中断，实测每秒只加约 700。不要小于 19，否则分频值超出 16 位。
- 若要亲眼看到除零异常，运行 `make run BUILD=build/selftest/divide_error SELFTEST=DIVIDE_ERROR`。异常面板会出现 `Exception 0 #DE divide error` 和出错指令的 `EIP`，用 `objdump -d build/selftest/divide_error/kernel.elf` 可以看到那个地址正是 `divl`。`selftest.c` 中用的是内联汇编：如果写成 C 的 `1 / zero`，clang 会把它优化成不用除法指令的比较，根本不会触发异常。
- 若要亲眼看到缺页异常，运行 `make run BUILD=build/selftest/unmapped SELFTEST=UNMAPPED`（读未映射的 `0xD0000000`）或 `make run BUILD=build/selftest/null_write SELFTEST=NULL_WRITE`（写空指针）。异常面板的第 2 行显示错误码，最后一行显示 CR2。
- 若要亲眼看到完整的寄存器面板，运行 `make run BUILD=build/selftest/register_dump SELFTEST=REGISTER_DUMP`。`selftest.c` 先把 EAX～EBP 设为 `0x11111111`～`0x77777777`，再执行 `ud2`。
- 若要看用户态程序越权，运行 `make run BUILD=build/selftest/user_write_kernel SELFTEST=USER_WRITE_KERNEL`（写内核变量 `timer_ticks`）或 `make run BUILD=build/selftest/user_privileged SELFTEST=USER_PRIVILEGED`（执行 `cli`）。第 7 行出现黄色的结束消息，任务 B 不再运行，第 6 行很快只剩 A；内核和任务 A 照常运行，不出现异常面板。
- 若要看用户程序正常退出，运行 `make run BUILD=build/selftest/user_exit SELFTEST=USER_EXIT`。用户程序写出 `bye` 后调用 `exit(7)`，第 7 行为 `task 2 exited with code 7`。
- 若要看内核拒绝用户传来的坏指针，运行 `make run BUILD=build/selftest/user_bad_pointer SELFTEST=USER_BAD_POINTER`。用户程序把内核地址交给 `write`，得到 -1 后写一个 `K`，第 6 行会出现一个 `K`。把 `syscall.c` 中 `sys_write()` 的 `user_range_ok()` 检查删掉再运行，`K` 就变成了 `X`，前面还多出一个内核代码的字节。
- 若要让任务 B 回到内核态，把 `sched.c` 中的 `init_user_task(2, …)` 改成 `init_task(2, task_main, cr3)`。第 6 行照常交替出现 A、B，但第 0 行不再出现 `U`。
- 若要换一个位置写 `V`，修改 `kernel.c` 中 `((volatile uint16_t *)VGA_ALIAS)[5]` 的下标，例如改成 `80 * 3`，`V` 就会出现在第 3 行行首。下标超过 2047（一页 4 KB 的范围）时会触发缺页，因为别名只映射了一页。
- 若要亲眼看到未填写的 IDT 表项被触发，运行 `make run BUILD=build/selftest/bad_gate SELFTEST=BAD_GATE`。`selftest.c` 执行 `int $0x30`，应看到 `Exception 13 #GP general protection`，`error=0x00000182`，即 `0x30 × 8 + 2`。

读取成功只表示扇区读取完成；这个最小实验尚未检查第二阶段和内核的内容或校验和。

`bits 16`、`bits 32` 和 `org` 是汇编指令，不是 CPU 执行的机器指令。`org` 告诉汇编器加载地址，不会在文件开头填充相应数量的字节。`loader.asm` 中远跳转之前按 16 位编码，`protected_mode` 之后按 32 位编码，与 CPU 当时所处的模式一致。`entry.asm` 和 `isr.asm` 编译成 ELF 目标文件，加载地址由链接脚本决定，所以不写 `org`。

## 文件

| 文件 | 作用 | 对应 Linux 0.11 |
| --- | --- | --- |
| `boot.asm` | 第一阶段：读盘、错误处理和跳转 | `boot/bootsect.s` |
| `loader.asm` | 第二阶段：打印 `B`，读入内核，建立 GDT、打开 A20、进入保护模式，打印 `P` 后跳进内核 | `boot/bootsect.s`、`boot/setup.s` |
| `entry.asm` | 内核入口 `_start`：设置内核栈、清零 `.bss`、打印 `K`，调用 `kmain()` | `boot/head.s` |
| `isr.asm` | 48 个中断入口和公共保存、恢复代码 | `kernel/asm.s`、`kernel/system_call.s` |
| `kernel.c` | `kmain()`：初始化各模块，打开中断 | `init/main.c` |
| `gdt.c` | 内核自己的 GDT | `boot/head.s` 中的 `setup_gdt` |
| `interrupt.c` | IDT、8259、中断分发和异常显示 | `kernel/traps.c` |
| `paging.c` | 页目录和页表，打开分页 | `boot/head.s` 中的 `setup_paging`、`mm/memory.c` |
| `timer.c` | 8253 定时器 | `kernel/sched.c` 中的 `sched_init()`、`do_timer()` |
| `syscall.c`、`syscall.h` | 系统调用：内核一侧的分发表，调用方使用的编号和包装函数 | `kernel/system_call.s`、`include/unistd.h` |
| `user.c` | 用户程序：任务 B 在用户态运行的 `user_main()` 和它的用户栈 | 无（Linux 0.11 的用户程序是磁盘上的可执行文件） |
| `sched.c` | 两个任务的 TSS、硬件任务切换和结束任务 | `kernel/sched.c` 中的 `schedule()`、`include/linux/sched.h` 中的 `switch_to`、`kernel/exit.c` 中的 `do_exit()` |
| `keyboard.c` | 键盘扫描码转字符并回显，支持 Shift 和 Caps Lock | `kernel/chr_drv/keyboard.S` |
| `vga.c` | 直接写 VGA 显存、移动硬件光标、整数转字符串 | `kernel/chr_drv/console.c` |
| `kernel.h`、`io.h` | 各模块共用的声明；端口读写 `inb`、`outb` | `include/asm/io.h` |
| `selftest.c`、`selftest.h` | 自检代码和编号，只在 `make test` 构建的自检内核中生效；用户态的自检写在 `user.c` 中 | 无 |
| `tests/test_kernel.py` | 自动化测试，由 `make test` 调用 | 无 |
| `linker.ld` | 内核链接脚本：链接地址 `0x10000` 和各段顺序 | `Makefile` 中的链接参数 |
| `Makefile` | 汇编、编译、链接、检查内核大小、打包和运行 QEMU | `Makefile`、`tools/build.c` |

生成的文件都在 `build/` 中：写进软盘的 `boot.bin`、`loader.bin`、`kernel.bin`，带符号的 `kernel.elf`，以及软盘镜像 `floppy.img`。自检内核在 `build/selftest/` 下。

参考：[SeaBIOS 读盘实现](https://github.com/coreboot/seabios/blob/master/src/disk.c)、[NASM 引导扇区填充与签名](https://www.nasm.us/doc/nasm14.html)、[QEMU 启动参数](https://www.qemu.org/docs/master/system/qemu-manpage.html)、[OSDev：A20 地址线](https://wiki.osdev.org/A20_Line)、[OSDev：GDT](https://wiki.osdev.org/Global_Descriptor_Table)、[LLD 链接脚本](https://lld.llvm.org/ELF/linker_script.html)、[OSDev：IDT](https://wiki.osdev.org/Interrupt_Descriptor_Table)、[OSDev：8259 PIC](https://wiki.osdev.org/8259_PIC)、[OSDev：PIT](https://wiki.osdev.org/Programmable_Interval_Timer)。
