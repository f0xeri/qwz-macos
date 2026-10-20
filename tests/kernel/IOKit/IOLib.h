#pragma once
#include <stddef.h>
void *IOMalloc(size_t);
void IOFree(void *, size_t);
void IOSleep(unsigned);
