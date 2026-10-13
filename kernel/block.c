#include "kernel.h"
#include "ppc.h"
#include "block.h"

// usb0, usb0p1, ram0. a partition just adds its start sector

static struct block devs[BLOCK_MAX];

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

int block_register(const char *name, uint64_t sectors, int (*read)(void *, uint64_t, int, void *),
		   int (*write)(void *, uint64_t, int, const void *), void *priv)
{
	for (int i = 0; i < BLOCK_MAX; i++)
		if (!devs[i].used) {
			struct block *b = &devs[i];
			memset(b, 0, sizeof *b);
			b->used = 1;
			strncpy(b->name, name, sizeof b->name - 1);
			b->sectors = sectors;
			b->parent = -1;
			b->read = read;
			b->write = write;
			b->priv = priv;
			return i;
		}
	return -1;
}

void block_remove(int dev)
{
	for (int i = 0; i < BLOCK_MAX; i++)
		if (devs[i].used && devs[i].parent == dev)
			devs[i].used = 0;
	devs[dev].used = 0;
}

struct block *block_get(int dev)
{
	return dev >= 0 && dev < BLOCK_MAX && devs[dev].used ? &devs[dev] : NULL;
}

int block_find(const char *name)
{
	for (int i = 0; i < BLOCK_MAX; i++)
		if (devs[i].used && !strcmp(devs[i].name, name))
			return i;
	return -1;
}

static int transfer(int dev, uint64_t lba, int count, void *buf, int write)
{
	struct block *b = block_get(dev);
	if (!b || lba + count > b->sectors)
		return -1;
	if (b->parent >= 0) {
		struct block *disk = block_get(b->parent);
		if (!disk)
			return -1;
		lba += b->start;
		b = disk;
	}
	return write ? b->write(b->priv, lba, count, buf) : b->read(b->priv, lba, count, buf);
}

int block_read(int dev, uint64_t lba, int count, void *buf)
{
	return transfer(dev, lba, count, buf, 0);
}

int block_write(int dev, uint64_t lba, int count, const void *buf)
{
	return transfer(dev, lba, count, (void *)buf, 1);
}

static int add_part(int disk, int n, uint8_t type, uint64_t start, uint64_t sectors)
{
	char name[12];
	snprintf(name, sizeof name, "%sp%d", devs[disk].name, n);
	int p = block_register(name, sectors, NULL, NULL, NULL);
	if (p >= 0) {
		devs[p].parent = disk;
		devs[p].start = start;
		devs[p].part = n;
		devs[p].type = type;
	}
	return p;
}

void block_scan(int disk)
{
	uint8_t mbr[SECTOR];
	if (block_read(disk, 0, 1, mbr) < 0 || mbr[510] != 0x55 || mbr[511] != 0xaa)
		return;
	for (int i = 0; i < 4; i++) {
		const uint8_t *e = mbr + 446 + i * 16;
		uint8_t type = e[4];
		uint64_t start = le32(e + 8), count = le32(e + 12);
		if (!type || type == 0x05 || type == 0x0f || !count || start + count > devs[disk].sectors)	// no extended partitions
			continue;
		add_part(disk, i + 1, type, start, count);
	}
}

// room after the last partition, starting on a 1 MB boundary like every tool does it
uint64_t block_free_tail(int disk, uint64_t *start)
{
	uint64_t end = 2048;
	for (int i = 0; i < BLOCK_MAX; i++)
		if (devs[i].used && devs[i].parent == disk && devs[i].start + devs[i].sectors > end)
			end = devs[i].start + devs[i].sectors;
	end = (end + 2047) & ~2047ull;
	*start = end;
	return devs[disk].sectors > end ? devs[disk].sectors - end : 0;
}

int block_add_partition(int disk, int slot, uint8_t type, uint64_t start, uint64_t sectors)
{
	uint8_t mbr[SECTOR];
	if (slot < 1 || slot > 4 || block_read(disk, 0, 1, mbr) < 0)
		return -1;
	if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
		memset(mbr, 0, sizeof mbr);
		mbr[510] = 0x55;
		mbr[511] = 0xaa;
	}
	uint8_t *e = mbr + 446 + (slot - 1) * 16;
	memset(e, 0, 16);
	e[4] = type;
	put_le32(e + 8, start);
	put_le32(e + 12, sectors);
	if (block_write(disk, 0, 1, mbr) < 0)
		return -1;
	return add_part(disk, slot, type, start, sectors);
}

static int ram_read(void *priv, uint64_t lba, int count, void *buf)
{
	memcpy(buf, (uint8_t *)priv + lba * SECTOR, (size_t)count * SECTOR);
	return count;
}

static int ram_write(void *priv, uint64_t lba, int count, const void *buf)
{
	memcpy((uint8_t *)priv + lba * SECTOR, buf, (size_t)count * SECTOR);
	return count;
}

// the memory only install. gone at power off
int ramdisk_create(uint64_t sectors)
{
	void *mem = page_alloc(sectors * SECTOR / PAGE);
	if (!mem)
		return -1;
	return block_register("ram0", sectors, ram_read, ram_write, mem);
}

void block_list(void)
{
	for (int i = 0; i < BLOCK_MAX; i++) {
		struct block *b = &devs[i];
		if (!b->used)
			continue;
		uint64_t mb = b->sectors / 2048;
		if (b->parent < 0)
			kprintf("%-8s disk       %6ld MB\n", b->name, (long)mb);
		else
			kprintf("%-8s partition  %6ld MB  type %02x  at sector %ld\n", b->name, (long)mb, b->type,
				(long)b->start);
	}
}
