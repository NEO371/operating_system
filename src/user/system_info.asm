BITS 32

%define SYS_EXIT 2
%define SYS_WRITE 3
%define SYS_GETPID 1

section .text
global _start
_start:
    mov eax, SYS_GETPID
    int 0x80
    test eax, eax
    jle .failed
    ; The loader uses the selected module name as argv[0].
    cmp dword [esp], 1
    jne .failed
    mov esi, [esp + 4]
    test esi, esi
    jz .failed
    mov edi, expected_name
    mov ecx, expected_name_end - expected_name
    repe cmpsb
    jne .failed
    cmp dword [esp + 8], 0
    jne .failed

    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, message
    mov edx, message_end - message
    int 0x80
    cmp eax, message_end - message
    jne .failed

    mov eax, SYS_EXIT
    mov ebx, 17
    int 0x80
.halt:
    jmp .halt
.failed:
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
    jmp .halt

section .rodata
expected_name: db "system-info", 0
expected_name_end:
message: db "Named ELF application launched", 10
message_end:
