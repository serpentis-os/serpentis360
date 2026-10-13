#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

#define BLOCK_MAX 16
#define SECTOR 512

// a disk or a partition. the driver only ever sees whole disk sector numbers
struct block {
	int used;
	char name[12];
	uint64_t sectors;
	int parent;		// -1 for a whole disk
	uint64_t start;
	int part;
	uint8_t type;
	int (*read)(void *priv, uint64_t lba, int count, void *buf);
	int (*write)(void *priv, uint64_t lba, int count, const void *buf);
	void *priv;
};

int block_register(const char *name, uint64_t sectors, int (*read)(void *, uint64_t, int, void *),
		   int (*write)(void *, uint64_t, int, const void *), void *priv);
void block_remove(int dev);
int block_read(int dev, uint64_t lba, int count, void *buf);
int block_write(int dev, uint64_t lba, int count, const void *buf);
struct block *block_get(int dev);
int block_find(const char *name);
void block_scan(int dev);
int block_add_partition(int disk, int slot, uint8_t type, uint64_t start, uint64_t sectors);
uint64_t block_free_tail(int disk, uint64_t *start);
int ramdisk_create(uint64_t sectors);
void block_list(void);

#endif
