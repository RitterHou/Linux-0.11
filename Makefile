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
ifneq ($(SELFTEST),)
KERNEL_CFLAGS += -DSELFTEST=SELFTEST_$(SELFTEST)
endif

KERNEL_OBJS := $(addprefix $(BUILD)/, entry.o isr.o kernel.o vga.o gdt.o \
	interrupt.o paging.o timer.o sched.o syscall.o keyboard.o selftest.o user.o)
KERNEL_HEADERS := kernel.h io.h syscall.h selftest.h

.PHONY: all run test clean

all: $(BUILD)/floppy.img $(BUILD)/kernel.elf

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

run: $(BUILD)/floppy.img
	$(QEMU) -name "Two-stage boot - C kernel" \
		-machine pc -accel tcg -m 16M \
		-drive file=$<,format=raw,if=floppy,index=0 \
		-boot order=a -display $(QEMU_DISPLAY) \
		-nic none -serial none -monitor none -no-reboot $(QEMU_FLAGS)

# 在无头 QEMU 中运行全部测试；自检内核由测试脚本按需构建到 build/selftest/ 下。
test: all
	QEMU="$(QEMU)" $(PYTHON) tests/test_kernel.py

clean:
	rm -rf build
