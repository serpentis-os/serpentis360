#include "kernel.h"
#include "ppc.h"
#include "usb.h"
#include "block.h"

// bulk only + scsi. the wrappers are little endian, the cdbs big endian

#define MAX_STICKS 4
#define CHUNK 64		// sectors per scsi command

struct stick {
	int used;
	struct usb_dev *dev;
	uint32_t tag;
	int block;
	uint64_t sectors;
};

static struct stick sticks[MAX_STICKS];
static int stick_number;

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void clear_halt(struct usb_dev *d, int ep, void *pipe)
{
	uint8_t setup[8] = {0x02, 0x01, 0, 0, ep, 0, 0, 0};
	ohci_control(d->hc, d->addr, d->low_speed, d->mps0, setup, NULL, 0);
	ohci_bulk_reset(pipe);
}

static void recover(struct usb_dev *d)
{
	uint8_t reset[8] = {0x21, 0xff, 0, 0, d->iface, 0, 0, 0};
	ohci_control(d->hc, d->addr, d->low_speed, d->mps0, reset, NULL, 0);
	clear_halt(d, d->ep_in | 0x80, d->bulk_in);
	clear_halt(d, d->ep_out, d->bulk_out);
}

// one scsi command. a stall on the data stage is fine, the status still comes after it.
// anything worse and the whole interface gets reset
static int command(struct stick *s, const uint8_t *cb, int cblen, int in, void *data, int len)
{
	struct usb_dev *d = s->dev;
	uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
	put_le32(cbw + 4, ++s->tag);
	put_le32(cbw + 8, len);
	cbw[12] = in ? 0x80 : 0;
	cbw[14] = cblen;
	memcpy(cbw + 15, cb, cblen);
	if (ohci_bulk(d->bulk_out, 0, cbw, 31) != 31) {
		recover(d);
		return -1;
	}
	if (len) {
		void *pipe = in ? d->bulk_in : d->bulk_out;
		int r = ohci_bulk(pipe, in, data, len);
		if (r == -4)
			clear_halt(d, in ? d->ep_in | 0x80 : d->ep_out, pipe);
		else if (r < 0) {
			recover(d);
			return -1;
		}
	}
	uint8_t csw[13];
	int r = ohci_bulk(d->bulk_in, 1, csw, 13);
	if (r == -4) {
		clear_halt(d, d->ep_in | 0x80, d->bulk_in);
		r = ohci_bulk(d->bulk_in, 1, csw, 13);
	}
	if (r != 13 || le32(csw) != 0x53425355 || le32(csw + 4) != s->tag) {
		recover(d);
		return -1;
	}
	if (csw[12] == 2)
		recover(d);
	return csw[12] ? -1 : 0;
}

// 3 tries per chunk, cheap sticks drop one now and then
static int rw(struct stick *s, uint64_t lba, int count, void *buf, int write)
{
	for (int done = 0; done < count;) {
		int n = count - done < CHUNK ? count - done : CHUNK;
		uint32_t at = lba + done;
		uint8_t cb[10] = {write ? 0x2a : 0x28, 0, at >> 24, at >> 16, at >> 8, at, 0, n >> 8, n, 0};
		int ok = -1;
		for (int tries = 0; tries < 3 && ok < 0; tries++)
			ok = command(s, cb, 10, !write, (uint8_t *)buf + (uint64_t)done * SECTOR, n * SECTOR);
		if (ok < 0)
			return -1;
		done += n;
	}
	return count;
}

static int stick_read(void *priv, uint64_t lba, int count, void *buf)
{
	return rw(priv, lba, count, buf, 0);
}

static int stick_write(void *priv, uint64_t lba, int count, const void *buf)
{
	return rw(priv, lba, count, (void *)buf, 1);
}

void storage_attach(struct usb_dev *d)
{
	if (d->subclass != 6 || d->protocol != 0x50 || !d->ep_in || !d->ep_out) {
		klog("usb: storage that is not scsi over bulk only, skipped\n");
		return;
	}
	struct stick *s = NULL;
	for (int i = 0; i < MAX_STICKS && !s; i++)
		if (!sticks[i].used)
			s = &sticks[i];
	if (!s)
		return;
	memset(s, 0, sizeof *s);
	s->dev = d;
	d->bulk_in = ohci_bulk_open(d->hc, d->addr, d->ep_in, d->low_speed, d->ep_in_mps);
	d->bulk_out = ohci_bulk_open(d->hc, d->addr, d->ep_out, d->low_speed, d->ep_out_mps);
	if (!d->bulk_in || !d->bulk_out)
		return;

	uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0}, info[36];
	command(s, inquiry, 6, 1, info, 36);

	// a stick that just woke up says not ready for a while, ask the sense to clear it
	uint8_t tur[6] = {0}, sense_cb[6] = {0x03, 0, 0, 0, 18, 0}, sense[18];
	for (int i = 0; i < 30 && command(s, tur, 6, 0, NULL, 0) < 0; i++) {
		command(s, sense_cb, 6, 1, sense, 18);
		sleep_ms(100);
	}

	uint8_t cap_cb[10] = {0x25}, cap[8];
	if (command(s, cap_cb, 10, 1, cap, 8) < 0) {
		klog("usb: stick does not report its size\n");
		return;
	}
	uint32_t last = (uint32_t)cap[0] << 24 | cap[1] << 16 | cap[2] << 8 | cap[3];
	uint32_t size = (uint32_t)cap[4] << 24 | cap[5] << 16 | cap[6] << 8 | cap[7];
	if (size != SECTOR) {
		klog("usb: stick with %d byte sectors, only 512 is supported\n", size);
		return;
	}
	s->sectors = (uint64_t)last + 1;
	s->used = 1;
	char name[12];
	snprintf(name, sizeof name, "usb%d", stick_number++);
	s->block = block_register(name, s->sectors, stick_read, stick_write, s);
	d->disk = s->block;
	char vendor[9], model[17];
	memcpy(vendor, info + 8, 8);
	vendor[8] = 0;
	memcpy(model, info + 16, 16);
	model[16] = 0;
	klog("usb: %s, %s %s, %ld MB\n", name, vendor, model, (long)(s->sectors / 2048));
	block_scan(s->block);
}

void storage_detach(struct usb_dev *d)
{
	for (int i = 0; i < MAX_STICKS; i++)
		if (sticks[i].used && sticks[i].dev == d) {
			block_remove(sticks[i].block);
			sticks[i].used = 0;
		}
}
