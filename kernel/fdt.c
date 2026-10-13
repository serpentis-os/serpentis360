#include "kernel.h"
#include "fdt.h"

// flattened device tree, big endian like the cpu so no byte swapping at all

#define FDT_MAGIC 0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

struct fdt_header {
	uint32_t magic, totalsize, off_struct, off_strings, off_rsvmap;
	uint32_t version, last_comp, boot_cpu, size_strings, size_struct;
};

int fdt_valid(uint64_t fdt)
{
	return fdt && ((struct fdt_header *)fdt)->magic == FDT_MAGIC;
}

int fdt_find(uint64_t fdt, const char *node, const char *prop, const void **value, int *len)
{
	if (!fdt_valid(fdt))
		return -1;
	struct fdt_header *h = (struct fdt_header *)fdt;
	const uint32_t *p = (const uint32_t *)(fdt + h->off_struct);
	const char *strings = (const char *)(fdt + h->off_strings);
	const char *names[16];
	int depth = 0;
	for (;;) {
		uint32_t token = *p++;
		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			if (depth < 16)
				names[depth] = name;
			depth++;
			p += (strlen(name) + 4) / 4;
		} else if (token == FDT_END_NODE) {
			depth--;
		} else if (token == FDT_PROP) {
			uint32_t size = p[0], name_off = p[1];
			const void *data = p + 2;
			p += 2 + (size + 3) / 4;
			if (strcmp(strings + name_off, prop))
				continue;
			if (node && (depth < 1 || depth > 16 || memcmp(names[depth - 1], node, strlen(node))))
				continue;
			*value = data;
			*len = size;
			return 0;
		} else if (token != FDT_NOP) {
			return -1;
		}
	}
}

uint64_t fdt_number(const void *value, int len)
{
	const uint32_t *v = value;
	return len >= 8 ? (uint64_t)v[0] << 32 | v[1] : v[0];
}
