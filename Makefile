NASM ?= nasm
CLANG ?= clang
LLD ?= ld.lld
QEMU ?= qemu-system-i386
QEMU_DISPLAY ?= cocoa
QEMU_FLAGS ?=
PYTHON ?= python3

# 构建目录。make test 会用 BUILD=build/selftest/<名称> 另外构建自检用的内核。
BUILD ?= build
# 非空时把 selftest.c 中对应的自检代码编进内核，例如 SELFTEST=DIVIDE_ERROR。
SELFTEST ?=

# 编译内核 C 代码：生成 32 位 x86 的 ELF 目标文件，不依赖 macOS 的头文件和库。
#   -ffreestanding          独立环境：没有标准库，main 也不是入口
#   -nostdlibinc            不搜索系统头文件，只用 clang 自带的 stdint.h 等
#   -fno-pic                按固定地址生成代码，与链接脚本中的 0x10000 一致
#   -fno-stack-protector    栈保护依赖运行库提供的函数，内核里没有
#   -mgeneral-regs-only     不使用 x87/MMX/SSE 寄存器，它们还没有初始化
KERNEL_CFLAGS := --target=i386-elf -march=i386 -std=c11 -O2 -Wall -Wextra \
	-ffreestanding -nostdlibinc -fno-builtin -fno-pic -fno-stack-protector \
	-fno-asynchronous-unwind-tables -mgeneral-regs-only
# 用户程序用同样的选项编译，链接到用户空间的底部（user/user.ld）。
USER_CFLAGS := $(KERNEL_CFLAGS)
ifneq ($(SELFTEST),)
KERNEL_CFLAGS += -DSELFTEST=SELFTEST_$(SELFTEST)
endif

KERNEL_OBJS := $(addprefix $(BUILD)/, entry.o isr.o kernel.o vga.o gdt.o \
	interrupt.o paging.o mem.o hd.o buffer.o fs.o file.o pipe.o exec.o tty.o timer.o sched.o syscall.o keyboard.o selftest.o user.o)
KERNEL_HEADERS := kernel.h io.h syscall.h selftest.h fs.h

.PHONY: all run test clean

all: $(BUILD)/floppy.img $(BUILD)/kernel.elf $(BUILD)/hd.img

$(BUILD):
	mkdir -p $@

$(BUILD)/boot.bin: boot.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# loader 要读多少个扇区，按 kernel.bin 的实际大小向上取整，在汇编时传进去。
$(BUILD)/loader.bin: loader.asm $(BUILD)/kernel.bin Makefile | $(BUILD)
	$(NASM) -f bin -DKERNEL_SECTORS=$$(( ($$(wc -c < $(BUILD)/kernel.bin) + 511) / 512 )) $< -o $@

# 内核的汇编文件（entry.asm、isr.asm）编译成 ELF 目标文件，地址由链接脚本决定。
$(BUILD)/%.o: %.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/%.o: %.c $(KERNEL_HEADERS) Makefile | $(BUILD)
	$(CLANG) $(KERNEL_CFLAGS) -c $< -o $@

# kernel.bin 是去掉 ELF 头、按链接地址排好的纯机器码，写进软盘；
# kernel.elf 保留符号，用于反汇编和以后用 gdb 调试。
$(BUILD)/kernel.bin: $(KERNEL_OBJS) linker.ld
	$(LLD) -m elf_i386 -T linker.ld --oformat=binary -o $@ $(KERNEL_OBJS)

$(BUILD)/kernel.elf: $(KERNEL_OBJS) linker.ld
	$(LLD) -m elf_i386 -T linker.ld -o $@ $(KERNEL_OBJS)

# 标准 1.44 MB 软盘：扇区 1 放 boot，扇区 2 放 loader，从扇区 3 起放 kernel。
$(BUILD)/floppy.img: $(BUILD)/boot.bin $(BUILD)/loader.bin $(BUILD)/kernel.bin Makefile
	dd if=/dev/zero of=$@ bs=512 count=2880
	dd if=$(BUILD)/boot.bin of=$@ bs=512 conv=notrunc
	dd if=$(BUILD)/loader.bin of=$@ bs=512 seek=1 conv=notrunc
	dd if=$(BUILD)/kernel.bin of=$@ bs=512 seek=2 conv=notrunc

# 用户程序：每个 user/名称.c 和 user/ulib.c 链接成 build/user/名称.elf，装进硬盘的 /bin/名称。
USER_PROGS := sh ls cat echo ps wc grep rm mkdir rmdir df hello check letter
USER_ELFS := $(addprefix $(BUILD)/user/, $(addsuffix .elf, $(USER_PROGS)))

$(BUILD)/user:
	mkdir -p $@

# 静态模式规则：macOS 自带的 make 3.81 会先匹配上面内核的 $(BUILD)/%.o 规则
USER_OBJS := $(addprefix $(BUILD)/user/, $(addsuffix .o, $(USER_PROGS) ulib))

$(USER_OBJS): $(BUILD)/user/%.o: user/%.c user/ulib.h syscall.h Makefile | $(BUILD)/user
	$(CLANG) $(USER_CFLAGS) -c $< -o $@

$(USER_ELFS): $(BUILD)/user/%.elf: $(BUILD)/user/%.o $(BUILD)/user/ulib.o user/user.ld
	$(LLD) -m elf_i386 -T user/user.ld -o $@ $< $(BUILD)/user/ulib.o

# 硬盘镜像：接在主 IDE 通道上，是一个 Minix v1 文件系统，内容是 rootfs/ 目录、
# /bin 下的用户程序，测试用的 600 KB 大文件 /big，以及两个终端设备文件：
# /dev/tty0 是控制台（第 8～18 行），/dev/tty1 是第 6 行。
ROOTFS_FILES := $(shell find rootfs -type f)

$(BUILD)/hd.img: tools/mkfs.py tools/mkbig.py $(ROOTFS_FILES) $(USER_ELFS) | $(BUILD)
	rm -rf $(BUILD)/rootfs
	cp -R rootfs $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/tmp
	for p in $(USER_PROGS); do cp $(BUILD)/user/$$p.elf $(BUILD)/rootfs/bin/$$p; done
	$(PYTHON) tools/mkbig.py $(BUILD)/rootfs/big
	$(PYTHON) tools/mkfs.py $@ $(BUILD)/rootfs --chr /dev/tty0=4,0 --chr /dev/tty1=4,1

run: $(BUILD)/floppy.img $(BUILD)/hd.img
	$(QEMU) -name "Two-stage boot - C kernel" \
		-machine pc -accel tcg -m 16M \
		-drive file=$<,format=raw,if=floppy,index=0 \
		-drive file=$(BUILD)/hd.img,format=raw,if=ide,index=0 \
		-boot order=a -display $(QEMU_DISPLAY) \
		-nic none -serial none -monitor none -no-reboot $(QEMU_FLAGS)

# 在无头 QEMU 中运行全部测试；自检内核由测试脚本按需构建到 build/selftest/ 下。
test: all
	QEMU="$(QEMU)" $(PYTHON) tests/test_kernel.py

clean:
	rm -rf build
