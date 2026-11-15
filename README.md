# MiniOS: runnable 32-bit x86 OS prototype

MiniOS is a freestanding, Multiboot 1 kernel for x86 protected mode. GRUB
requests an 800x600, 32-bit linear RGB framebuffer; the kernel draws a teal
desktop, boot console, and live Task & Memory Manager directly into it.

The manager reads CS, DS, SS, and ESP from the CPU, decodes the GDT base for
the active code/data selectors, and reports the linker-measured text/data/BSS
image sizes together with current stack usage.

## Quick start (WSL / Ubuntu)

```sh
sudo apt update
sudo apt install -y build-essential gcc-multilib nasm grub-pc-bin xorriso mtools qemu-system-x86
```

`gcc-multilib` supplies 32-bit `libgcc`, which is needed by the freestanding
link (integer conversion and memory-bar arithmetic may require compiler helper
routines). No hosted C library is linked.

From the project directory, build the bootable ISO and launch it in QEMU:

```sh
cd /mnt/d/projects/operatingSystem/prototype2
make clean
make
make run
```

`make run` opens the graphical QEMU window through WSLg (`-display gtk`). Boot
messages stay in the Ubuntu terminal; the OS desktop appears in the separate
QEMU window. Use `Ctrl+C` in the terminal (or close the QEMU window) to shut
the guest down. To validate the Multiboot image
without running it, use `make check`. To run the complete automated suite, use
`make release-check`; it builds separate test images and boots them in QEMU.

On boot, sign in with the displayed demo credentials `demo` / `1234`.
The desktop shortcuts are `1` for the DBMS terminal, `2` for Task & Memory
Manager, `3` for the network status placeholder, `4` for Snake, `5` for the
PCI Hardware Inventory, and `6` for MiniAI. MiniAI accepts typed questions and
prints local answers about MiniOS, memory, CPU, and Snake. It is an offline
rule-based demo, not a connected generative AI service. In Snake, use the arrow
keys to steer, Space to pause, Enter to restart after a collision, and Escape
to return to the desktop.
Application windows can be repositioned by dragging their title bars with the
mouse. Single-click a desktop icon to open it; use the title-bar close button
or Escape to return to the desktop.

The DBMS terminal runs a bounded SQL engine and displays query results in the
terminal. Commands are case-insensitive; finish each command with Enter. Start
with `HELP`, then try:

```sql
CREATE TABLE people (id INT, name TEXT);
INSERT INTO people VALUES (1, 'Ada');
SELECT * FROM people;
SELECT COUNT(*) FROM people;
SHOW TABLES;
UPDATE people SET name = 'Ada Lovelace' WHERE id = 1;
DELETE FROM people WHERE id = 1;
DROP TABLE people;
```

Each query changes or reads the table data; inserts enforce unique IDs, updates
and deletes match rows by ID, and `SELECT *` prints the stored ID/name rows.
Tables have fixed limits of four tables and eight rows per table. `make run`
attaches a dedicated FAT16 disk image at `dbms-data.img`; database changes are
saved to `DBMS.DAT` and survive reboots. The image is created on the first run
and `make clean` leaves it intact. The SQL engine is still a kernel-side
prototype and does not run as an isolated user process.

Press `Ctrl+Y` to show or hide Task & Resource Manager from any active screen.
Its bounded event ledger records user-mode syscall numbers, arguments, results,
actual heap allocation attempts/results/frees from the heap entry points, and
application request/result/release records correlated by request ID. The
application view reports requested payload bytes;
the kernel heap view reports allocator payload bytes and tracks direct kernel
heap allocations separately from Snake-owned payload. Current and cumulative
byte counts are shown separately. The ledger keeps the newest 512 events in
RAM; `Clear View` only advances the display cutoff, and paused history is
anchored while new events arrive. `Resume Live View` follows the newest events.
Timestamps are PIT ticks since boot. The resource ledger remains in RAM and its
history is lost at reboot; the DBMS data disk is persistent. The kernel exposes
no CPU-use percentage. The clickable `PROCESSES / GANTT` tab shows process
states and a rolling 40-sample PIT timeline. PID 0 is the kernel-hosted desktop
context; ring-3 scheduler workers appear with their current state, and the
chart records their actual timer slices. Snake and the DBMS terminal are
kernel-hosted applications rather than separate scheduled processes.

## Build checks

Run the controlled protection, scheduling, PCI, and storage checks in
QEMU-only images with `make fault-check`, `make process-check`,
`make scheduler-check`, `make pci-check`, `make keyboard-check`, and
`make storage-check`. `make dbms-check` verifies that the DBMS creates and
inserts a row on one QEMU boot, then reloads and selects it on the next boot.
The first deliberately writes to virtual address zero and read-only kernel
data. The second checks isolated ring-3 address spaces and page reclamation.
The third verifies PIT-driven preemption, the ELF user program, `SPAWN`/`WAIT`,
user-fault isolation, and process cleanup. The PCI check verifies discovery of
QEMU's virtual display through read-only PCI configuration reads. Each test
exits through QEMU's `isa-debug-exit`; test artifacts stay under `build/fault/`,
`build/process/`, `build/scheduler/`, `build/pci/`, and `build/storage*/`
instead of replacing the normal image.
The storage check reads a multi-cluster FAT16 file from both an MBR partition
and a superfloppy image, then confirms that an out-of-range partition is
rejected. All three virtual disks are generated under `build/` and attached in
QEMU snapshot mode.
Run `make release-check` for the complete gate: image and Multiboot validation,
the ISA-debug-exit suites, DBMS persistence across QEMU boots, and a timed
normal-image QEMU boot that checks
the scheduler and desktop-loop serial markers. The smoke log is saved at
`build/smoke/serial.log`.
The keyboard check injects `4` and Escape through QEMU's monitor and verifies
that Snake opens and returns to the desktop; its serial output is saved at
`build/keyboard/serial.log`.

`make` and `make iso` create `mini-os.iso`; `make check` asks GRUB to validate
the Multiboot header. Run the OS with `make run`. It creates or reuses the
project's `dbms-data.img` FAT16 disk, attaches it as the primary IDE disk, and
boots the ISO in QEMU with software emulation (`-accel tcg`), GTK display
(`-display gtk`), and networking disabled (`-nic none`). Database writes stay
in the project-local virtual disk; no host disk or physical-device passthrough
is configured. Serial diagnostics are sent to the terminal (`-serial stdio`).

The kernel installs an IDT before GUI initialization. Fatal CPU exceptions are
reported through COM1 with the vector, error code, instruction address, code
selector, and flags, then halt the guest. The PIC is remapped and the PIT
provides 100 Hz ticks; timer, PS/2 keyboard, and PS/2 mouse IRQs are enabled. Keyboard
make codes are translated into a bounded queue, and the main loop sleeps with
`hlt` between events. Snake advances from timer ticks instead of a busy-wait
delay. Other hardware IRQ lines stay masked.

The kernel also parses GRUB's Multiboot memory map and initializes a 4 KiB
physical-page allocator. It reserves low firmware memory, the loaded kernel,
the Multiboot info/map and modules, and the framebuffer; allocated pages are
zeroed, and the boot path checks allocation/free behavior before enabling the
allocator. A bounded kernel heap uses contiguous page-backed arenas, splits
free blocks, coalesces them on release, and returns empty arenas to the page
allocator. Its boot check covers small and multi-page requests, zero/oversized
requests, and invalid/double frees. The heap is single-core and not safe for
interrupt-context or concurrent use yet.

The kernel enables 4 KiB paging with identity mappings only for pages known to
the memory manager, leaves virtual page zero unmapped, marks kernel text and
rodata read-only, and maps the framebuffer uncached. Two boot-time ring-3 tasks
share read-only code but receive separate page directories, private writable
data/stack pages, and separate kernel stacks. Each uses the DPL-3 `int 0x80`
gate. The syscall ABI in `include/syscall.h` provides `GETPID`, `EXIT` (status in EBX),
and `WRITE` (fd in EBX, user buffer in ECX, byte count in EDX). `WRITE` accepts
serial fd 1 and at most 256 bytes. It validates every covered page as
user-readable before accessing the buffer, returning `-9` for another fd,
`-22` for oversized requests, and `-14` for an invalid range. The scheduler
QEMU check exercises a valid write plus supervisor, unmapped, cross-page, and
wrapping pointers from ring 3.

The boot ISO includes two named ELF32 applications as GRUB Multiboot modules.
The kernel registers bounded module names from GRUB, rejects duplicate names,
and can load the default app or resolve another module by name. It validates
each ELF header and load segments, copies supported
`PT_LOAD` ranges into private user pages, zeroes BSS, and enters its ELF entry
point at ring 3. It accepts static 32-bit little-endian executables with at
most 32 mapped pages, enforces read-only versus writable mappings, and checks
that the entry point is inside file-backed executable text. It does not handle
dynamic linking, interpreter segments, shared libraries, or NX protection. The
fixture in `src/user/hello.asm` verifies initialized data, BSS zero-fill, checked
output, startup arguments, heap growth, and clean exit.

The prototype also has `SPAWN`, `SPAWN_NAMED`, and blocking `WAIT` (child PID
in EBX, exit status returned in EAX). `SPAWN` accepts a linked user-text entry
in ECX, or ECX=0 to load the default ELF module as a new child.
`SPAWN_NAMED` takes a module-name pointer in ECX and resolves it against the
registered GRUB module names. Both ELF paths use EBX for an optional packed
argument vector and EDX for its byte length. Each
argument is NUL-terminated, followed by one extra NUL sentinel; up to seven
arguments and 128 total bytes are accepted. Empty arguments, malformed
sentinels, invalid pointers, and oversized vectors are rejected. The child
starts with a basic i386 `argc`/`argv`/empty-`envp` stack. `BRK` (request in
EBX, resulting break in EAX; zero queries the current break) grows or shrinks
the process heap inside its user address-space limit. Each child gets a
separate address space and stack; `WAIT` blocks its parent and reclaims child
pages on completion. QEMU checks a normal child exit, an isolated protection
fault, the default ELF child, and a separately named `system-info` ELF child.
The parent receives statuses 42, 142 (128 plus page-fault vector 14), 0, and
17. The static scheduler table allows at most three concurrent tasks. This is
not yet a general process API, and NX support is absent. Unsupported syscalls
return `-38`.

PCI discovery reads configuration headers without probing BARs or enabling
devices. The inventory displays up to 20 of the first 32 discovered PCI
functions. The network screen is a status placeholder: this kernel does not yet
include an Ethernet/Wi-Fi driver, TCP/IP stack, DHCP, or DNS, and the QEMU
profile intentionally has no network device.
Storage support has bounded primary-master ATA PIO sector reads and writes,
including cache-flushed FAT16 root-file updates for the DBMS data file.
The fixed-capacity block-device registry decouples sector I/O from the ATA
driver and checks request bounds. FAT16
accepts superfloppy media and primary MBR partitions, while rejecting
partition extents outside the disk; it does not follow extended/logical
partitions. QEMU storage checks use generated disposable images. The registry
is synchronous and has no locking for concurrent callers. The current kernel
is a learning prototype, not production-ready. It still lacks scalable process
management, a general file namespace, networking, and the device drivers needed
for a general-purpose OS. Run it in QEMU as described above.

The default compiler is host GCC with `-m32`. To use a dedicated bare-metal
toolchain instead, run:

```sh
make clean
make CROSS=i686-elf- run
```

## Project map

```text
prototype2/
├── boot/grub/          GRUB boot menu configuration
├── docs/               Roadmap and project documentation
├── include/            Shared kernel headers and embedded font
├── src/
│   ├── arch/x86/        Multiboot entry, interrupt/context assembly, linker script
│   ├── kernel/          Kernel, drivers, GUI, memory, scheduler, and DBMS
│   └── user/            ELF32 user programs and their linker script
├── tests/              Optional fault-test C and assembly fixtures
├── tools/              Disk-image preparation scripts
├── build/               Generated object files, ISOs, and test logs
├── Makefile             Build, run, and QEMU check targets
└── README.md            Setup, run instructions, and feature guide
```

The build reads source from these folders and keeps generated files under
`build/`. The normal bootable image and persistent `dbms-data.img` remain in
the project root; `make clean` removes build products without deleting DBMS
data.
