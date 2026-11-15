BITS 32

%define USER_CODE_SELECTOR 0x1B
%define USER_DATA_SELECTOR 0x23
%define USER_DATA_ADDRESS 0x40000000
%define SYS_GETPID 1
%define SYS_EXIT 2
%define SYS_WRITE 3
%define SYS_SPAWN 4
%define SYS_WAIT 5
%define SYS_SPAWN_NAMED 7
%define SYS_SMOKE_VERIFY 127
%define ERR_FAULT 14
%define ERR_INVAL 22
%define ERR_NOENT 2
%define USER_COUNTER_LIMIT 5000000
%define USER_STACK_TOP 0xBFFFE000

section .text
global ring3_enter
global ring3_enter_at
global ring3_leave
global ring3_saved_esp
global process_context_switch
global process_restore_irq_frame
global process_restore_syscall_frame

ring3_enter:
    mov edx, user_test_entry
    mov ecx, USER_STACK_TOP
    jmp ring3_enter_common

ring3_enter_at:
    mov edx, [esp + 4]
    mov ecx, [esp + 8]

ring3_enter_common:
    pushfd
    push ebp
    push ebx
    push esi
    push edi
    mov [ring3_saved_esp], esp

    mov ax, USER_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword USER_DATA_SELECTOR
    push ecx
    pushfd
    pop eax
    push eax
    push dword USER_CODE_SELECTOR
    push edx
    iretd

; Restore the kernel C call frame after the syscall dispatcher chooses exit.
ring3_leave:
    mov esp, [ring3_saved_esp]
    pop edi
    pop esi
    pop ebx
    pop ebp
    popfd
    ret

; Save a kernel continuation and restore another. void switch(uint32_t *old,
; uint32_t new_esp); the frame is EDI, ESI, EBX, EBP, EFLAGS, return EIP.
process_context_switch:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    pushfd
    push ebp
    push ebx
    push esi
    push edi
    mov [eax], esp
    mov esp, edx
    pop edi
    pop esi
    pop ebx
    pop ebp
    popfd
    ret

; Restore a saved IRQ register frame followed by a privilege-return frame.
process_restore_irq_frame:
    mov esp, [esp + 4]
    mov ax, USER_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    iretd

; A blocked task resumes at the saved int 0x80 frame: discard vector/error.
process_restore_syscall_frame:
    mov esp, [esp + 4]
    mov ax, USER_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    iretd

section .bss
align 4
ring3_saved_esp: resd 1

section .usertext
align 16
global user_test_entry
extern _text_start
user_test_entry:
    inc dword [USER_DATA_ADDRESS]
    mov ebx, [USER_DATA_ADDRESS]
    mov eax, SYS_SMOKE_VERIFY
    int 0x80
    mov eax, [_text_start]
    mov eax, 2
    int 0x80
.halt:
    jmp .halt

align 16
global scheduler_user_entry
scheduler_user_entry:
    mov eax, SYS_GETPID
    int 0x80
    mov [USER_DATA_ADDRESS + 4], eax
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_ADDRESS + 8
    mov edx, 21
    int 0x80
    cmp eax, 21
    jne .failed

    ; Kernel text is supervisor-only even though it is mapped in this CR3.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, _text_start
    mov edx, 4
    int 0x80
    cmp eax, -ERR_FAULT
    jne .failed

    ; Reject a range that crosses into an unmapped user page.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0x40000FFF
    mov edx, 2
    int 0x80
    cmp eax, -ERR_FAULT
    jne .failed

    ; Reject a range whose 32-bit address arithmetic would wrap.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0xFFFFFFFE
    mov edx, 4
    int 0x80
    cmp eax, -ERR_FAULT
    jne .failed
.spin:
    inc dword [USER_DATA_ADDRESS]
    cmp dword [USER_DATA_ADDRESS], USER_COUNTER_LIMIT
    jb .spin
    cmp dword [USER_DATA_ADDRESS + 4], 1
    jne .exit_success
    mov eax, SYS_SPAWN
    mov ecx, scheduler_child_entry
    xor ebx, ebx
    xor edx, edx
    int 0x80
    test eax, eax
    jle .failed
    mov ebx, eax
    mov eax, SYS_WAIT
    int 0x80
    cmp eax, 42
    jne .failed
    mov eax, SYS_SPAWN
    mov ecx, scheduler_fault_child_entry
    xor ebx, ebx
    xor edx, edx
    int 0x80
    test eax, eax
    jle .failed
    mov ebx, eax
    mov eax, SYS_WAIT
    int 0x80
    cmp eax, 142
    jne .failed
    ; Invalid pointers, malformed packed vectors, and oversized vectors reject.
    mov eax, SYS_SPAWN
    xor ecx, ecx
    mov ebx, 0xFFFFFFFE
    mov edx, 4
    int 0x80
    cmp eax, -ERR_FAULT
    jne .failed
    mov eax, SYS_SPAWN
    xor ecx, ecx
    mov ebx, scheduler_bad_argument
    mov edx, scheduler_bad_argument_end - scheduler_bad_argument
    int 0x80
    cmp eax, -ERR_INVAL
    jne .failed
    mov eax, SYS_SPAWN
    xor ecx, ecx
    mov ebx, scheduler_empty_argument
    mov edx, scheduler_empty_argument_end - scheduler_empty_argument
    int 0x80
    cmp eax, -ERR_INVAL
    jne .failed
    mov eax, SYS_SPAWN
    xor ecx, ecx
    mov ebx, scheduler_too_many_arguments
    mov edx, scheduler_too_many_arguments_end - scheduler_too_many_arguments
    int 0x80
    cmp eax, -ERR_INVAL
    jne .failed
    mov eax, SYS_SPAWN
    xor ecx, ecx
    mov ebx, USER_DATA_ADDRESS + 8
    mov edx, 129
    int 0x80
    cmp eax, -ERR_INVAL
    jne .failed
    ; ECX=0 asks the kernel to instantiate its validated Multiboot ELF image.
    mov eax, SYS_SPAWN
    xor ecx, ecx
    mov ebx, scheduler_elf_arguments
    mov edx, scheduler_elf_argument_end - scheduler_elf_arguments
    int 0x80
    test eax, eax
    jle .failed
    mov ebx, eax
    mov eax, SYS_WAIT
    int 0x80
    test eax, eax
    jne .failed
    ; Named spawning validates the module-name pointer and reports missing apps.
    mov eax, SYS_SPAWN_NAMED
    mov ecx, 0xFFFFFFFE
    xor ebx, ebx
    xor edx, edx
    int 0x80
    cmp eax, -ERR_FAULT
    jne .failed
    mov eax, SYS_SPAWN_NAMED
    mov ecx, scheduler_missing_module
    xor ebx, ebx
    xor edx, edx
    int 0x80
    cmp eax, -ERR_NOENT
    jne .failed
    ; Launch an ELF module other than the default boot application.
    mov eax, SYS_SPAWN_NAMED
    mov ecx, scheduler_named_module
    xor ebx, ebx
    xor edx, edx
    int 0x80
    test eax, eax
    jle .failed
    mov ebx, eax
    mov eax, SYS_WAIT
    int 0x80
    cmp eax, 17
    jne .failed
.exit_success:
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.exited:
    jmp .exited
.failed:
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
    jmp .exited

align 16
global scheduler_child_entry
scheduler_child_entry:
    mov eax, SYS_GETPID
    int 0x80
    mov [USER_DATA_ADDRESS + 4], eax
    mov eax, SYS_EXIT
    mov ebx, 42
    int 0x80
.child_halt:
    jmp .child_halt

align 16
global scheduler_fault_child_entry
scheduler_fault_child_entry:
    mov eax, SYS_GETPID
    int 0x80
    mov [USER_DATA_ADDRESS + 4], eax
    mov eax, [_text_start]
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
.fault_halt:
    jmp .fault_halt

align 4
scheduler_elf_arguments: db "qemu-argument", 0, "second-argument", 0, 0
scheduler_elf_argument_end:
scheduler_bad_argument: db "missing-final-sentinel", 0
scheduler_bad_argument_end:
scheduler_empty_argument: db "first", 0, 0, "second", 0, 0
scheduler_empty_argument_end:
scheduler_too_many_arguments: db "1", 0, "2", 0, "3", 0, "4", 0
    db "5", 0, "6", 0, "7", 0, "8", 0, 0
scheduler_too_many_arguments_end:
scheduler_named_module: db "system-info", 0
scheduler_missing_module: db "not-installed", 0

global scheduler_user_terminated
scheduler_user_terminated:
.halt:
    jmp .halt
