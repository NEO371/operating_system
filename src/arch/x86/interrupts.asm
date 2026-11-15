BITS 32

section .text
extern exception_dispatch
extern irq_dispatch
global idt_load

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp exception_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; CPU already pushed this exception's error code.
    push dword %1
    jmp exception_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 128

%macro IRQ_STUB 1
global irq%1
irq%1:
    pusha
    mov eax, esp
    push eax
    push dword %1
    call irq_dispatch
    add esp, 8
    mov esp, eax
    popa
    iretd
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

global isr_default
isr_default:
    push dword 0
    push dword 255
    jmp exception_common

exception_common:
    cld
    pusha
    ; Keep the interrupted selectors; the C handler runs with kernel data.
    xor eax, eax
    mov ax, ds
    push eax
    xor eax, eax
    mov ax, es
    push eax
    xor eax, eax
    mov ax, fs
    push eax
    xor eax, eax
    mov ax, gs
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    lea eax, [esp + 16]
    push eax
    call exception_dispatch
    add esp, 4
    pop eax
    mov gs, ax
    pop eax
    mov fs, ax
    pop eax
    mov es, ax
    pop eax
    mov ds, ax
    popa
    add esp, 8
    iretd

idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

section .rodata
align 4
global isr_stub_table
isr_stub_table:
    %assign vector 0
    %rep 32
        dd isr%+vector
        %assign vector vector + 1
    %endrep

align 4
global irq_stub_table
irq_stub_table:
    dd irq0, irq1, irq2, irq3
    dd irq4, irq5, irq6, irq7
    dd irq8, irq9, irq10, irq11
    dd irq12, irq13, irq14, irq15
