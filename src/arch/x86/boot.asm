; Multiboot v1 entry point and protected-mode descriptor-table support.
BITS 32

%define MULTIBOOT_HEADER_MAGIC 0x1BADB002
%define MULTIBOOT_FLAGS        0x00000007 ; align modules, memory map, video

section .multiboot
align 4
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_FLAGS
    dd -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_FLAGS)
    dd 0                         ; graphics mode request
    dd 800                       ; width
    dd 600                       ; height
    dd 32                        ; bits per pixel

section .rodata
align 8
bootstrap_gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF         ; 0x08: base 0, 4 GiB ring-0 code
    dq 0x00CF92000000FFFF         ; 0x10: base 0, 4 GiB ring-0 data
bootstrap_gdt_end:

bootstrap_gdtr:
    dw bootstrap_gdt_end - bootstrap_gdt - 1
    dd bootstrap_gdt

section .bss
align 16
global stack_bottom
global stack_top
stack_bottom:
    resb 16384                    ; 16 KiB kernel stack
stack_top:

align 16
global ring3_kernel_stack_bottom
global ring3_kernel_stack_top
ring3_kernel_stack_bottom:
    resb 16384                    ; Separate privilege-transition stack.
ring3_kernel_stack_top:

align 4
boot_magic: resd 1
boot_info:  resd 1

section .text
global _start
global gdt_flush
extern gdt_init
extern kernel_main

_start:
    cli
    mov [boot_magic], eax         ; Multiboot magic from GRUB
    mov [boot_info], ebx          ; physical/linear multiboot_info pointer

    ; Do not depend on GRUB's descriptor table after this point.
    lgdt [bootstrap_gdtr]
    jmp 0x08:.segments_ready
.segments_ready:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_top
    mov ebp, esp

    call gdt_init
    push dword [boot_info]
    push dword [boot_magic]
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

; void gdt_flush(const struct gdt_ptr *gdtr)
; The C caller has already built the three descriptors in memory.
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
