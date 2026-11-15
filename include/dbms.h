#ifndef MINI_OS_DBMS_H
#define MINI_OS_DBMS_H

#include <stdint.h>

/* Bounded SQL engine backed by the FAT16 root file DBMS.DAT. */
void dbms_init(void);
int dbms_storage_is_ready(void);
void dbms_execute(const char *query, char *output, uint32_t output_capacity);
int dbms_persistence_self_check(void);

#endif
