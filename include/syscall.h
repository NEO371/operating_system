#ifndef MINI_OS_SYSCALL_H
#define MINI_OS_SYSCALL_H

/* 32-bit ring-3 ABI: syscall number in EAX, arguments in EBX/ECX/EDX,
 * integer result in EAX. SPAWN takes entry/module in ECX; for ELF children,
 * EBX/EDX optionally point to a packed argv vector of NUL-ended strings plus
 * a final NUL sentinel, with EDX counting all bytes. ECX=0 selects the
 * first validated boot ELF module. SPAWN_NAMED takes a user pointer to a
 * NUL-terminated module name in ECX and the same optional argv vector in
 * EBX/EDX. WAIT takes a child PID in EBX.
 * The int 0x80 gate is the ABI entry point. */
enum minios_syscall_number {
    MINIOS_SYS_GETPID = 1u,
    MINIOS_SYS_EXIT = 2u,
    MINIOS_SYS_WRITE = 3u,
    MINIOS_SYS_SPAWN = 4u,
    MINIOS_SYS_WAIT = 5u,
    MINIOS_SYS_BRK = 6u,
    MINIOS_SYS_SPAWN_NAMED = 7u
};

#define MINIOS_ERR_BADF 9u
#define MINIOS_ERR_CHILD 10u
#define MINIOS_ERR_NOENT 2u
#define MINIOS_ERR_AGAIN 11u
#define MINIOS_ERR_NOMEM 12u
#define MINIOS_ERR_FAULT 14u
#define MINIOS_ERR_INVAL 22u
#define MINIOS_ERR_NOSYS 38u
/* Private boot self-test; this number is not part of the user syscall ABI. */
#define MINIOS_SYS_SMOKE_VERIFY 0x7Fu

#endif
