BITS 32

section .text
global paging_test_write_null
global paging_test_null_resume
global paging_test_write_rodata
global paging_test_rodata_resume

paging_test_write_null:
    mov dword [0], 0x51A7E001
paging_test_null_resume:
    ret

paging_test_write_rodata:
    mov dword [paging_test_ro_word], 0x51A7E002
paging_test_rodata_resume:
    ret

section .rodata
align 4
global paging_test_ro_word
paging_test_ro_word:
    dd 0x13579BDF
