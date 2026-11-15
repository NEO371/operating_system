# MiniOS development roadmap

MiniOS is an educational 32-bit x86 kernel. “Modern OS standard” is a large
engineering target; the stages below make progress measurable and avoid
presenting prototype features as production-ready.

## Current baseline

- GRUB Multiboot v1 boot, kernel/user GDT descriptors with a 32-bit TSS, and a
  framebuffer desktop.
- Interrupt-driven PS/2 keyboard input and a single-threaded Snake application.
- CPU exception diagnostics, a physical-page allocator, and identity paging
  with page zero unmapped and kernel text/rodata read-only.
- Kernel segment and image metrics; a small ring-3 scheduler and process
  lifecycle prototype; a bounded named static ELF32 Multiboot-module loader;
  ATA PIO with bounded FAT16 root-file reads/writes for the persistent DBMS;
  no general process manager or real networking.
- QEMU-only run target with TCG and networking disabled, plus a project-local
  persistent virtual disk for DBMS data.

## Milestones

1. **Kernel diagnostics and fault handling** — install exception gates, report
   CPU faults over a QEMU serial console, document build assumptions, and add
   basic image checks. Complete.
2. **Memory management** — validate and consume the Multiboot memory map, add a
   physical page allocator, enable paging, and add a bounded kernel heap.
3. **Interrupts and time** — extend the IDT with PIC/PIT handling, replace busy
   waits with timer ticks, and move keyboard input to a buffered
   interrupt-driven path.
4. **Processes and protection** — add a TSS, ring-3 transition, context switch,
   scheduler, and a narrow syscall boundary before claiming a task manager.
5. **Devices and storage** — enumerate PCI safely, implement virtual block and
   display/input drivers, then add a filesystem with explicit bounds checks.
6. **Networking** — select a QEMU NIC model, implement its driver and a small
   Ethernet/IPv4 stack, then add DHCP/DNS and a testable socket API. Networking
   stays disabled in the default QEMU profile until this milestone is ready.
7. **Release quality** — automate build and image checks, add fault and device
   tests in QEMU, document limitations, and only then assess hardening and
   broader hardware support.

## Progress

- **Milestone 1 complete:** IDT gates and CPU exception stubs are installed;
  fatal faults report the vector, error code, EIP, CS, and EFLAGS over COM1.
  The QEMU smoke boot confirmed that IDT setup reaches the kernel desktop.
- **Milestone 2 complete:** validated Multiboot memory-map parsing, boot and
  kernel reservations, a zeroing physical-page allocator, contiguous page
  allocation, and a QEMU boot-time allocator self-check. The kernel enables
  4 KiB identity paging over known pages, leaves page zero unmapped, marks
  kernel text/rodata read-only with CR0.WP, and maps framebuffer pages uncached.
  The bounded heap uses contiguous page arenas, splits and coalesces blocks,
  rejects invalid/double frees, and returns empty arenas to the page allocator.
  Its QEMU self-check passed small and multi-page allocation cases. The guest
  remained running without a page fault during the smoke interval. The
  Multiboot v1 structure has compile-time ABI offset and size assertions.
- **Milestone 3 complete:** the PIC is remapped, the PIT runs at 100 Hz, and
  only IRQ0/IRQ1 are enabled. The keyboard IRQ feeds a set-1 scancode decoder
  and bounded ring buffer; the main loop uses `hlt`, and Snake timing uses PIT
  ticks. QEMU confirmed 100 timer ticks and delivered monitor-injected `4` and
  Escape events to the desktop loop. Other IRQ lines remain masked; spurious
  IRQ handling and synchronization remain outstanding.
- **Protection checks complete:** `make fault-check` builds an isolated image,
  triggers a null-page write and a kernel `.rodata` write, verifies CR2 and the
  page-fault present/write/user bits, resumes from both expected faults, and
  exits through QEMU's `isa-debug-exit`. The target passed.
- **Release gate added:** `make release-check` runs image and Multiboot checks,
  the fault, process, and scheduler suites, and a bounded QEMU boot of the
  normal image. The normal boot must report successful child cleanup and 100
  PIT ticks before the gate passes. A clean release-gate run passed. This
  improves repeatability; it does not close the device, storage, networking,
  or security-hardening milestones below.
- **Milestone 4 scheduler and lifecycle prototype:** the kernel saves and
  restores interrupt contexts, switches CR3 and TSS kernel stacks on PIT
  interrupts, and round-robins two fixed ring-3 tasks in separate page
  directories. A minimal `int 0x80` ABI implements `GETPID`, `EXIT`, and serial
  `WRITE`. The scheduler skips exited tasks, checks their status, and reclaims
  their user pages. QEMU verified both processes were preempted while runnable, completed
  independently, and returned control to the desktop. The `WRITE` syscall
  validates each user mapping before dereferencing a buffer; QEMU checks
  supervisor, unmapped cross-page, and wrapping pointers. `SPAWN` creates a
  separate child address space and stack for an entry in the linked user-text
  section. `SPAWN` with ECX=0 instantiates the validated ELF module as a child.
  Blocking `WAIT` resumes the parent with the child's exit status and
  reclaims the child's page tables and user pages. A user processor exception
  terminates only that task and wakes a waiting parent with status 128 plus the
  exception vector. `make scheduler-check` verifies normal exit, a deliberate
  protection fault, status propagation, ELF child launch, and reclamation in
  QEMU; the process and fault checks pass as well. This remains a three-slot
  scheduler demonstration with a limited process API. The ELF32 `ET_EXEC`
  module loads into private user pages and runs at its ELF entry point. The
  loader checks segment bounds, page limits, writable permissions, and that the
  entry is file-backed executable text; NX is not available yet. The QEMU boot
  prints output from the standalone ring-3 application and verifies clean exit
  and page reclamation. `SPAWN` now passes up to seven bounded arguments
  through a basic `argc`/`argv`/empty-`envp` stack, and rejects invalid
  pointers, empty entries, malformed sentinels, and oversized vectors. `BRK`
  lets a process query, grow, and shrink its private
  heap within the user address-space limit; the ELF QEMU fixture writes to a
  newly mapped page and then releases it. The kernel registers bounded GRUB
  module names, and `SPAWN_NAMED` resolves a user request to the matching ELF.
  A second `system-info` ELF verifies its `argv[0]`, output, distinct exit
  status, and reaping in QEMU. The loader skips zero-sized `PT_LOAD` entries
  emitted by some linkers while continuing to reject unsupported non-empty
  segments. Dynamic linking, interpreter segments, and NX remain unsupported.
- **Milestone 5 in progress:** the primary-master ATA PIO driver performs
  bounded 28-bit LBA sector reads and writes through a fixed-capacity generic
  block-device registry. Writes flush the drive cache. FAT16 mounts a registered
  512-byte device, supports bounded 8.3 root-file reads and writes on superfloppy
  media or primary MBR partitions, and the DBMS persists `DBMS.DAT` on its own
  FAT16 virtual disk.
  QEMU checks use generated disposable images, cover a file spanning two
  clusters, and reject a partition extending past the disk. PCI
  mechanism #1 discovery now scans configuration
  space without writing BARs or enabling devices, and the desktop's Hardware
  Inventory shows the first 20 discovered functions. `make pci-check` verifies
  that QEMU's virtual PCI display device is discovered. Storage still lacks
  extended/logical partition support, directory traversal, a filesystem cache,
  and a general filesystem namespace. FAT16 writes are limited to root files
  and are not exposed through user syscalls; the registry is synchronous and
  not synchronized for concurrent callers. PCI bridge-aware enumeration and
  drivers for additional devices remain future work.
- **Next core milestones (prioritized for the required project):**
  1. Expose checked root-file operations to ring 3 and add directory traversal
     and a general filesystem namespace.
  2. Move the persistent DBMS into a ring-3 process and add indexes and
     transactional semantics.
  3. Add a QEMU NIC driver (start with a model QEMU can emulate), Ethernet,
     ARP, IPv4, and UDP/TCP. Expose a bounded socket API and verify traffic
     between the guest and a host-side test peer.
  4. Build the network client as a real user process using
     kernel syscalls; show kernel-owned process, memory, disk, and network
     information in the framebuffer desktop.

  PCI bridge traversal and additional hardware support remain useful, but are
  secondary to writable storage and networking for the requested project.
