NASM ?= nasm
CLANG ?= clang
LLD ?= ld.lld
QEMU ?= qemu-system-i386
QEMU_DISPLAY ?= cocoa
QEMU_FLAGS ?=

# 内核在软盘上占用的扇区数。loader 按这个数读取，打包时检查 kernel.bin 不超出。
KERNEL_SECTORS := 16

# 编译内核 C 代码：生成 32 位 x86 的 ELF 目标文件，不依赖 macOS 的头文件和库。
#   -ffreestanding          独立环境：没有标准库，main 也不是入口
#   -nostdlibinc            不搜索系统头文件，只用 clang 自带的 stdint.h 等
#   -fno-pic                按固定地址生成代码，与链接脚本中的 0x10000 一致
#   -fno-stack-protector    栈保护依赖运行库提供的函数，内核里没有
#   -mgeneral-regs-only     不使用 x87/MMX/SSE 寄存器，它们还没有初始化
KERNEL_CFLAGS := --target=i386-elf -march=i386 -std=c11 -O2 -Wall -Wextra \
	-ffreestanding -nostdlibinc -fno-builtin -fno-pic -fno-stack-protector \
	-fno-asynchronous-unwind-tables -mgeneral-regs-only

KERNEL_OBJS := build/entry.o build/kernel.o

.PHONY: all run clean

all: build/floppy.img build/kernel.elf

build:
	mkdir -p build

build/boot.bin: boot.asm | build
	$(NASM) -f bin $< -o $@

build/loader.bin: loader.asm Makefile | build
	$(NASM) -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@

build/entry.o: entry.asm | build
	$(NASM) -f elf32 $< -o $@

build/kernel.o: kernel.c Makefile | build
	$(CLANG) $(KERNEL_CFLAGS) -c $< -o $@

# kernel.bin 是去掉 ELF 头、按链接地址排好的纯机器码，写进软盘；
# kernel.elf 保留符号，用于反汇编和以后用 gdb 调试。
build/kernel.bin: $(KERNEL_OBJS) linker.ld
	$(LLD) -m elf_i386 -T linker.ld --oformat=binary -o $@ $(KERNEL_OBJS)

build/kernel.elf: $(KERNEL_OBJS) linker.ld
	$(LLD) -m elf_i386 -T linker.ld -o $@ $(KERNEL_OBJS)

# 标准 1.44 MB 软盘：扇区 1 放 boot，扇区 2 放 loader，从扇区 3 起放 kernel。
build/floppy.img: build/boot.bin build/loader.bin build/kernel.bin Makefile
	@size=$$(wc -c < build/kernel.bin | tr -d ' '); \
	if [ $$size -gt $$(( $(KERNEL_SECTORS) * 512 )) ]; then \
		echo "kernel.bin 有 $$size 字节，超出 KERNEL_SECTORS=$(KERNEL_SECTORS) 个扇区"; \
		exit 1; \
	fi
	dd if=/dev/zero of=$@ bs=512 count=2880
	dd if=build/boot.bin of=$@ bs=512 conv=notrunc
	dd if=build/loader.bin of=$@ bs=512 seek=1 conv=notrunc
	dd if=build/kernel.bin of=$@ bs=512 seek=2 conv=notrunc

run: build/floppy.img
	$(QEMU) -name "Two-stage boot - C kernel" \
		-machine pc -accel tcg -m 16M \
		-drive file=$<,format=raw,if=floppy,index=0 \
		-boot order=a -display $(QEMU_DISPLAY) \
		-nic none -serial none -monitor none -no-reboot $(QEMU_FLAGS)

clean:
	rm -rf build
