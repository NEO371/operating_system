# Host GCC with multilib works by default.  A bare-metal toolchain is selected
# explicitly with: make CROSS=i686-elf- run
CROSS ?=
CC := $(CROSS)gcc
AS := nasm
LD := $(CROSS)ld
GRUB_MKRESCUE ?= grub-mkrescue
GRUB_FILE ?= grub-file
QEMU ?= qemu-system-i386
ARCH_DIR := src/arch/x86
KERNEL_SRC := src/kernel
USER_SRC := src/user
INCLUDE_DIR := include

BUILD := build
ISO_ROOT := $(BUILD)/isodir
SMOKE_LOG := $(BUILD)/smoke/serial.log
KERNEL := $(BUILD)/kernel.elf
ISO := mini-os.iso
DBMS_DISK := dbms-data.img
OBJECTS := $(BUILD)/boot.o $(BUILD)/gdt.o $(BUILD)/gui.o $(BUILD)/input.o \
           $(BUILD)/interrupts.o $(BUILD)/interrupts_asm.o $(BUILD)/memory.o \
           $(BUILD)/paging.o $(BUILD)/heap.o $(BUILD)/pci.o $(BUILD)/block.o \
           $(BUILD)/ata_pio.o $(BUILD)/fat16.o \
           $(BUILD)/dbms.o \
           $(BUILD)/resource_monitor.o \
           $(BUILD)/mouse.o \
           $(BUILD)/ring3.o $(BUILD)/ring3_asm.o \
           $(FAULT_TEST_OBJECTS) $(BUILD)/kernel.o

CFLAGS := -std=gnu11 -m32 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-pie -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -I$(INCLUDE_DIR)
LDFLAGS := -m32 -nostdlib -ffreestanding -fno-pie -no-pie -T $(ARCH_DIR)/linker.ld

.PHONY: all iso check run dbms-disk fault-check process-check scheduler-check pci-check keyboard-check desktop-check storage-check dbms-check smoke-check release-check clean

all: iso

iso: $(ISO)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: $(ARCH_DIR)/boot.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/interrupts_asm.o: $(ARCH_DIR)/interrupts.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/ring3_asm.o: $(ARCH_DIR)/ring3.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/fault_tests_asm.o: tests/fault_tests.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/fault_tests.o: tests/fault_tests.c $(wildcard $(INCLUDE_DIR)/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(KERNEL_SRC)/%.c $(wildcard $(INCLUDE_DIR)/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJECTS) $(ARCH_DIR)/linker.ld
	$(CC) $(LDFLAGS) $(OBJECTS) -lgcc -o $@

$(BUILD)/user-demo.o: $(USER_SRC)/hello.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user-demo.elf: $(BUILD)/user-demo.o $(USER_SRC)/linker.ld
	$(LD) -m elf_i386 -T $(USER_SRC)/linker.ld $< -o $@

$(BUILD)/system-info.o: $(USER_SRC)/system_info.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/system-info.elf: $(BUILD)/system-info.o $(USER_SRC)/linker.ld
	$(LD) -m elf_i386 -T $(USER_SRC)/linker.ld $< -o $@

$(ISO): $(KERNEL) $(BUILD)/user-demo.elf $(BUILD)/system-info.elf boot/grub/grub.cfg
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNEL) $(ISO_ROOT)/boot/kernel.elf
	cp $(BUILD)/user-demo.elf $(ISO_ROOT)/boot/user-demo.elf
	cp $(BUILD)/system-info.elf $(ISO_ROOT)/boot/system-info.elf
	cp boot/grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_ROOT)

check: $(KERNEL)
	$(GRUB_FILE) --is-x86-multiboot $(KERNEL)

dbms-disk:
	python3 tools/make_dbms_disk.py $(DBMS_DISK)

run: $(ISO) dbms-disk
	$(QEMU) -display gtk -accel tcg -nic none -serial stdio -m 128M -vga std -boot order=d -drive file=$(DBMS_DISK),format=raw,if=ide,index=0 -cdrom $(ISO)

fault-check:
	$(MAKE) BUILD=build/fault ISO=build/fault/mini-os.iso FAULT_TEST_OBJECTS='build/fault/fault_tests.o build/fault/fault_tests_asm.o' CFLAGS='$(CFLAGS) -DMINIOS_FAULT_TEST' build/fault/mini-os.iso
	@timeout 15s $(QEMU) -accel tcg -snapshot -nic none -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -m 128M -vga std -cdrom build/fault/mini-os.iso; status=$$?; test $$status -eq 33

process-check:
	$(MAKE) BUILD=build/process ISO=build/process/mini-os.iso CFLAGS='$(CFLAGS) -DMINIOS_PROCESS_TEST' build/process/mini-os.iso
	@timeout 15s $(QEMU) -accel tcg -snapshot -nic none -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -m 128M -vga std -cdrom build/process/mini-os.iso; status=$$?; test $$status -eq 33

scheduler-check:
	$(MAKE) BUILD=build/scheduler ISO=build/scheduler/mini-os.iso CFLAGS='$(CFLAGS) -DMINIOS_SCHEDULER_TEST' build/scheduler/mini-os.iso
	@timeout 15s $(QEMU) -accel tcg -snapshot -nic none -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -m 128M -vga std -cdrom build/scheduler/mini-os.iso; status=$$?; test $$status -eq 33

pci-check:
	$(MAKE) BUILD=build/pci ISO=build/pci/mini-os.iso CFLAGS='$(CFLAGS) -DMINIOS_PCI_TEST' build/pci/mini-os.iso
	@timeout 15s $(QEMU) -accel tcg -snapshot -nic none -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -m 128M -vga std -cdrom build/pci/mini-os.iso; status=$$?; test $$status -eq 33

keyboard-check: $(ISO)
	mkdir -p build/keyboard
	@rm -f build/keyboard/serial.log
	@{ sleep 3; printf 'sendkey 4\n'; sleep 1; printf 'sendkey esc\n'; sleep 1; printf 'quit\n'; } | timeout 10s $(QEMU) -accel tcg -snapshot -nic none -serial file:$(abspath build/keyboard/serial.log) -monitor stdio -display none -m 128M -vga std -cdrom $(ISO) >/dev/null
	@grep -q "MiniOS: Snake app launched from keyboard shortcut." build/keyboard/serial.log
	@grep -q "MiniOS: returned to desktop." build/keyboard/serial.log
	@echo "MiniOS: QEMU keyboard shortcut and Escape return passed."

desktop-check: $(ISO)
	mkdir -p build/desktop
	@rm -f build/desktop/serial.log
	@{ sleep 3; printf 'sendkey 4\n'; sleep 1; printf 'sendkey ctrl-y\n'; sleep 1; printf 'sendkey ctrl-y\n'; sleep 1; printf 'sendkey esc\n'; sleep 1; printf 'mouse_move 300 200\n'; printf 'mouse_button 1\n'; sleep 1; printf 'mouse_button 0\n'; sleep 1; printf 'quit\n'; } | timeout 15s $(QEMU) -accel tcg -snapshot -nic none -serial file:$(abspath build/desktop/serial.log) -monitor stdio -display none -m 128M -vga std -cdrom $(ISO) >/dev/null
	@grep -q "MiniOS: PS/2 mouse enabled." build/desktop/serial.log
	@grep -q "MiniOS: Snake app launched from keyboard shortcut." build/desktop/serial.log
	@grep -q "MiniOS: Task Manager opened by Ctrl+Y." build/desktop/serial.log
	@grep -q "MiniOS: Task Manager hidden by Ctrl+Y." build/desktop/serial.log
	@grep -q "MiniOS: mouse event delivered to desktop loop." build/desktop/serial.log
	@echo "MiniOS: Ctrl+Y toggle and PS/2 mouse event checks passed."

build/storage/test-disk.img: tools/make_storage_test_disk.py
	python3 $< $@

build/storage/test-superfloppy.img: tools/make_storage_test_disk.py
	python3 $< $@ --superfloppy

build/storage-reject/test-invalid-partition.img: tools/make_storage_test_disk.py
	python3 $< $@ --invalid-partition

storage-check: build/storage/test-disk.img build/storage/test-superfloppy.img build/storage-reject/test-invalid-partition.img
	$(MAKE) BUILD=build/storage ISO=build/storage/mini-os.iso CFLAGS='$(CFLAGS) -DMINIOS_STORAGE_TEST' build/storage/mini-os.iso
	@timeout 20s $(QEMU) -accel tcg -snapshot -nic none -boot d -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file=build/storage/test-disk.img,format=raw,if=ide,index=0,snapshot=on -m 128M -vga std -cdrom build/storage/mini-os.iso; status=$$?; test $$status -eq 33
	@timeout 20s $(QEMU) -accel tcg -snapshot -nic none -boot d -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file=build/storage/test-superfloppy.img,format=raw,if=ide,index=0,snapshot=on -m 128M -vga std -cdrom build/storage/mini-os.iso; status=$$?; test $$status -eq 33
	$(MAKE) BUILD=build/storage-reject ISO=build/storage-reject/mini-os.iso CFLAGS='$(CFLAGS) -DMINIOS_STORAGE_REJECT_TEST' build/storage-reject/mini-os.iso
	@timeout 20s $(QEMU) -accel tcg -snapshot -nic none -boot d -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file=build/storage-reject/test-invalid-partition.img,format=raw,if=ide,index=0,snapshot=on -m 128M -vga std -cdrom build/storage-reject/mini-os.iso; status=$$?; test $$status -eq 33

dbms-check:
	mkdir -p build/dbms-check
	rm -f build/dbms-check/dbms-data.img
	python3 tools/make_dbms_disk.py build/dbms-check/dbms-data.img
	$(MAKE) BUILD=build/dbms-check ISO=build/dbms-check/mini-os.iso CFLAGS='$(CFLAGS) -DMINIOS_DBMS_TEST' build/dbms-check/mini-os.iso
	@timeout 30s $(QEMU) -accel tcg -nic none -boot order=d -serial file:$(abspath build/dbms-check/first-boot.log) -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file=build/dbms-check/dbms-data.img,format=raw,if=ide,index=0 -m 128M -vga std -cdrom build/dbms-check/mini-os.iso; status=$$?; test $$status -eq 33
	@timeout 30s $(QEMU) -accel tcg -nic none -boot order=d -serial file:$(abspath build/dbms-check/second-boot.log) -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file=build/dbms-check/dbms-data.img,format=raw,if=ide,index=0 -m 128M -vga std -cdrom build/dbms-check/mini-os.iso; status=$$?; test $$status -eq 33
	@grep -q "MiniOS: DBMS create, insert, disk reload, and SELECT passed." build/dbms-check/first-boot.log
	@grep -q "MiniOS: DBMS create, insert, disk reload, and SELECT passed." build/dbms-check/second-boot.log
	@echo "MiniOS: DBMS persistence across QEMU boots passed."

smoke-check: $(ISO)
	mkdir -p $(dir $(SMOKE_LOG))
	@timeout 8s $(QEMU) -accel tcg -snapshot -nic none -serial file:$(abspath $(SMOKE_LOG)) -display none -m 128M -vga std -cdrom $(ISO) || test $$? -eq 124
	@grep -q "MiniOS: scheduled process spawn, wait, exit, and reap passed." $(SMOKE_LOG)
	@grep -q "MiniOS: PIT tick delivery verified (100 ticks)." $(SMOKE_LOG)
	@echo "MiniOS: normal QEMU boot reached the desktop loop."

release-check: iso check fault-check process-check scheduler-check pci-check keyboard-check desktop-check storage-check dbms-check smoke-check

clean:
	rm -rf $(BUILD) $(ISO)
