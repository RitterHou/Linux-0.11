NASM ?= nasm
QEMU ?= qemu-system-i386
QEMU_DISPLAY ?= cocoa
QEMU_FLAGS ?=

# 内核在软盘上占用的扇区数。loader 按这个数读取，打包时检查 kernel.bin 不超出。
KERNEL_SECTORS := 16

.PHONY: all run clean

all: build/floppy.img

build:
	mkdir -p build

build/boot.bin: boot.asm | build
	$(NASM) -f bin $< -o $@

build/loader.bin: loader.asm Makefile | build
	$(NASM) -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@

build/kernel.bin: kernel.asm | build
	$(NASM) -f bin $< -o $@

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
	$(QEMU) -name "Two-stage boot - ABPK" \
		-machine pc -accel tcg -m 16M \
		-drive file=$<,format=raw,if=floppy,index=0 \
		-boot order=a -display $(QEMU_DISPLAY) \
		-nic none -serial none -monitor none -no-reboot $(QEMU_FLAGS)

clean:
	rm -rf build
