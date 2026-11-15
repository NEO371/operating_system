#ifndef MINI_OS_HEAP_H
#define MINI_OS_HEAP_H

#include <stddef.h>

/* Single-core kernel heap. Not safe for concurrent/interrupt-context use yet. */
int heap_init(void);
int heap_is_ready(void);
void *kmalloc(size_t size);
int kfree(void *pointer);

#endif
