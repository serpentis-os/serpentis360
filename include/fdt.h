#ifndef FDT_H
#define FDT_H

#include <stdint.h>

// read only and only what the kernel asks for, the timebase and how much memory there is
int fdt_valid(uint64_t fdt);
int fdt_find(uint64_t fdt, const char *node, const char *prop, const void **value, int *len);
uint64_t fdt_number(const void *value, int len);

#endif
