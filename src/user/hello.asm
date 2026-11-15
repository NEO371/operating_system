BITS 32

%define SYS_GETPID 1
%define SYS_EXIT 2
%define SYS_WRITE 3
%define SYS_BRK 6

section .text
 global _start
_start:
    mov eax, SYS_GETPID
    int 0x80
    test eax, eax
    jle .failed

    ; Initial stack follows the 32-bit argc/argv/envp process-entry layout.
    cmp dword [esp], 1
    je .check_argv0
    cmp dword [esp], 3
    jne .failed
    mov esi, [esp + 8]
    test esi, esi
    jz .failed
    mov edi, expected_argument
    mov ecx, expected_argument_end - expected_argument
    repe cmpsb
    jne .failed
    mov esi, [esp + 12]
    test esi, esi
    jz .failed
    mov edi, expected_argument2
    mov ecx, expected_argument2_end - expected_argument2
    repe cmpsb
    jne .failed
    cmp dword [esp + 16], 0
    jne .failed
    cmp dword [esp + 20], 0
    jne .failed
    jmp .check_argv0
.check_argv0:
    mov esi, [esp + 4]
    test esi, esi
    jz .failed
    mov edi, expected_argv0
    mov ecx, expected_argv0_end - expected_argv0
    repe cmpsb
    jne .failed
    cmp dword [esp + 8], 0
    jne .check_data
    cmp dword [esp + 12], 0
    jne .failed

    ; The loader must zero the PT_LOAD tail described by p_memsz - p_filesz.
.check_data:
    cmp dword [bss_probe], 0
    jne .failed
    mov dword [bss_probe], 0x12345678
    cmp dword [initialized_value], 0xC0DEF00D
    jne .failed

    ; Grow by one page, touch it, then shrink and reap the heap page.
    mov eax, SYS_BRK
    xor ebx, ebx
    int 0x80
    test eax, eax
    jle .failed
    mov ebp, eax
    lea ebx, [ebp + 0x1000]
    mov eax, SYS_BRK
    int 0x80
    cmp eax, ebx
    jne .failed
    mov dword [ebp], 0x5A17B00B
    cmp dword [ebp], 0x5A17B00B
    jne .failed
    mov eax, SYS_BRK
    mov ebx, ebp
    int 0x80
    cmp eax, ebp
    jne .failed

    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, message
    mov edx, message_end - message
    int 0x80
    cmp eax, message_end - message
    jne .failed

    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.halt:
    jmp .halt
.failed:
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
    jmp .halt

section .rodata
message: db "ELF user application says hello", 10
message_end:
expected_argv0: db "user-demo", 0
expected_argv0_end:
expected_argument: db "qemu-argument", 0
expected_argument_end:
expected_argument2: db "second-argument", 0
expected_argument2_end:

section .data
initialized_value: dd 0xC0DEF00D

section .bss
align 4
bss_probe: resd 1
