NASM ?= nasm
QEMU ?= qemu-system-i386
QEMU_DISPLAY ?= cocoa
QEMU_FLAGS ?=

.PHONY: all run clean

all: build/floppy.img

build:
	mkdir -p build

build/boot.bin: boot.asm | build
	$(NASM) -f bin $< -o $@

build/loader.bin: loader.asm | build
	$(NASM) -f bin $< -o $@

# 标准 1.44 MB 软盘：第一个扇区放 boot，第二个扇区放 loader。
build/floppy.img: build/boot.bin build/loader.bin Makefile
	dd if=/dev/zero of=$@ bs=512 count=2880
	dd if=build/boot.bin of=$@ bs=512 conv=notrunc
	dd if=build/loader.bin of=$@ bs=512 seek=1 conv=notrunc

run: build/floppy.img
	$(QEMU) -name "Two-stage boot - AB" \
		-machine pc -accel tcg -m 16M \
		-drive file=$<,format=raw,if=floppy,index=0 \
		-boot order=a -display $(QEMU_DISPLAY) \
		-nic none -serial none -monitor none -no-reboot $(QEMU_FLAGS)

clean:
	rm -rf build
