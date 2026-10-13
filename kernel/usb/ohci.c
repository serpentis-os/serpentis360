#include "kernel.h"
#include "ppc.h"
#include "usb.h"

// ohci. polled from the usb thread every 10 ms, no interrupts. all of it little endian

#define HC_REVISION 0x00
#define HC_CONTROL 0x04
#define HC_COMMAND 0x08
#define HC_INT_DISABLE 0x14
#define HC_HCCA 0x18
#define HC_CONTROL_HEAD 0x20
#define HC_BULK_HEAD 0x28
#define HC_FM_INTERVAL 0x34
#define HC_PERIODIC_START 0x40
#define HC_RH_A 0x48
#define HC_RH_STATUS 0x50
#define HC_RH_PORT 0x54

#define CTRL_CBSR 3
#define CTRL_PLE (1 << 2)
#define CTRL_CLE (1 << 4)
#define CTRL_BLE (1 << 5)
#define CTRL_OPERATIONAL (2 << 6)
#define CTRL_IR (1 << 8)
#define CMD_RESET 1
#define CMD_CLF 2
#define CMD_BLF 4
#define CMD_OCR 8

#define ED_SKIP (1u << 14)
#define TD_SETUP (0u << 19)
#define TD_OUT (1u << 19)
#define TD_IN (2u << 19)
#define TD_NO_INT (7u << 21)
#define TD_DATA0 (2u << 24)
#define TD_DATA1 (3u << 24)
#define TD_ROUND (1u << 18)
#define CC_NOT_ACCESSED 0xfu
#define MAX_HC 4

struct ed {
	uint32_t flags, tail, head, next;
};

struct td {
	uint32_t flags, cbp, next, be;
};

struct pipe {
	int hc;
	struct ed *ed;
	struct td *active, *spare;
	uint8_t *buf;
	int len;
};

struct ohci {
	volatile uint32_t *regs;
	uint32_t *hcca;
	struct ed *control;
	struct ed *periodic;
	struct ed *bulk;
	struct td *tds;
	uint8_t *setup;
	uint8_t *data;
	int ports;
	volatile int lock;	// one control transfer at a time, the ed and tds are shared
};

static struct ohci hcs[MAX_HC];
static int hc_count;

static uint32_t le(uint32_t v)
{
	return __builtin_bswap32(v);
}

static uint32_t rd(struct ohci *h, int reg)
{
	return le(h->regs[reg / 4]);
}

static void wr(struct ohci *h, int reg, uint32_t v)
{
	h->regs[reg / 4] = le(v);
}

static uint32_t phys(const void *p)
{
	return (uint32_t)(uint64_t)p;
}

static void delay_us(uint64_t us)
{
	uint64_t end = mftb() + platform_tb_freq() / 1000000 * us;
	while (mftb() < end)
		;
}

// take it from the firmware, reset, one page for the hcca, the ed heads and the control tds,
// one page for data. then power every port
int ohci_init(uint64_t base)
{
	if (hc_count == MAX_HC)
		return -1;
	struct ohci *h = &hcs[hc_count];
	h->regs = (volatile uint32_t *)base;
	if ((rd(h, HC_REVISION) & 0xff) != 0x10) {
		klog("usb: no ohci at %lx\n", base);
		return -1;
	}

	if (rd(h, HC_CONTROL) & CTRL_IR) {
		wr(h, HC_COMMAND, CMD_OCR);
		for (int i = 0; i < 1000 && (rd(h, HC_CONTROL) & CTRL_IR); i++)
			delay_us(1000);
	}

	uint32_t interval = rd(h, HC_FM_INTERVAL) & 0x3fff;
	if (!interval)
		interval = 0x2edf;
	wr(h, HC_COMMAND, CMD_RESET);
	for (int i = 0; i < 100 && (rd(h, HC_COMMAND) & CMD_RESET); i++)
		delay_us(10);

	uint8_t *page = (uint8_t *)as_page_alloc();
	uint8_t *page2 = (uint8_t *)as_page_alloc();
	if (!page || !page2)
		return -1;
	h->hcca = (uint32_t *)page;
	h->control = (struct ed *)(page + 256);
	h->periodic = (struct ed *)(page + 272);
	h->bulk = (struct ed *)(page + 288);
	h->tds = (struct td *)(page + 512);
	h->setup = page + 1024;
	h->data = page2;

	h->control->flags = le(ED_SKIP);
	h->periodic->flags = le(ED_SKIP);
	h->bulk->flags = le(ED_SKIP);
	for (int i = 0; i < 32; i++)
		h->hcca[i] = le(phys(h->periodic));

	wr(h, HC_HCCA, phys(h->hcca));
	wr(h, HC_CONTROL_HEAD, phys(h->control));
	wr(h, HC_BULK_HEAD, phys(h->bulk));
	wr(h, HC_INT_DISABLE, 0xc000007f);
	wr(h, HC_FM_INTERVAL, interval | ((6 * (interval - 210) / 7) << 16) | 0x80000000);
	wr(h, HC_PERIODIC_START, interval * 9 / 10);
	wr(h, HC_CONTROL, CTRL_OPERATIONAL | CTRL_CLE | CTRL_BLE | CTRL_PLE | CTRL_CBSR);

	uint32_t a = rd(h, HC_RH_A);
	h->ports = a & 0xff;
	wr(h, HC_RH_STATUS, 1 << 16);
	for (int p = 0; p < h->ports; p++)
		wr(h, HC_RH_PORT + p * 4, 1 << 8);
	delay_us(((a >> 24) & 0xff) * 2000 + 20000);
	klog("usb: ohci %d at %lx, %d ports\n", hc_count, base, h->ports);
	return hc_count++;
}

int ohci_count(void)
{
	return hc_count;
}

int ohci_ports(int hc)
{
	return hcs[hc].ports;
}

uint32_t ohci_port_status(int hc, int port)
{
	return rd(&hcs[hc], HC_RH_PORT + port * 4);
}

void ohci_port_write(int hc, int port, uint32_t value)
{
	wr(&hcs[hc], HC_RH_PORT + port * 4, value);
}

static void td_fill(struct td *t, uint32_t flags, void *buf, int len, struct td *next)
{
	t->flags = le(flags | (CC_NOT_ACCESSED << 28));
	t->cbp = len ? le(phys(buf)) : 0;
	t->be = len ? le(phys(buf) + len - 1) : 0;
	t->next = le(phys(next));
}

static int td_cc(struct td *t)
{
	return le(t->flags) >> 28;
}

// busy waits on the timebase, has to work before the scheduler is up
int ohci_control(int hc, int addr, int low, int mps, const uint8_t setup[8], void *data, int len)
{
	struct ohci *h = &hcs[hc];
	int in = setup[0] & 0x80;
	if (len > PAGE)
		return -1;
	lock_take(&h->lock);
	memcpy(h->setup, setup, 8);
	if (!in && len)
		memcpy(h->data, data, len);

	struct td *t = h->tds;
	int n = 0;
	td_fill(&t[n], TD_SETUP | TD_DATA0 | TD_NO_INT, h->setup, 8, &t[n + 1]);
	n++;
	if (len) {
		td_fill(&t[n], (in ? TD_IN : TD_OUT) | TD_DATA1 | TD_NO_INT | TD_ROUND, h->data, len, &t[n + 1]);
		n++;
	}
	td_fill(&t[n], (in && len ? TD_OUT : TD_IN) | TD_DATA1 | TD_NO_INT, NULL, 0, &t[n + 1]);
	struct td *tail = &t[n + 1];
	memset(tail, 0, sizeof *tail);

	struct ed *e = h->control;
	e->flags = le(addr | (low ? 1 << 13 : 0) | (mps << 16) | ED_SKIP);
	e->tail = le(phys(tail));
	e->head = le(phys(&t[0]));
	e->next = 0;
	__asm__ volatile("sync" : : : "memory");
	e->flags &= ~le(ED_SKIP);
	wr(h, HC_COMMAND, CMD_CLF);

	uint64_t end = mftb() + platform_tb_freq() / 2;
	int done = 0;
	while (mftb() < end) {
		uint32_t head = le(e->head);
		if ((head & ~0xfu) == phys(tail) || (head & 1)) {
			done = 1;
			break;
		}
		delay_us(50);
	}
	e->flags |= le(ED_SKIP);
	int failed = !done;
	for (int i = 0; i <= n && !failed; i++) {
		int cc = td_cc(&t[i]);
		if (cc != 0 && !(cc == 9 && in && i == 1))	// 9 is a short read, fine on the way in
			failed = 1;
	}
	e->head = 0;
	if (failed) {
		lock_give(&h->lock);
		return -1;
	}
	int moved = len;
	if (len && in) {
		uint32_t cbp = le(t[1].cbp);
		if (cbp)
			moved = cbp - phys(h->data);
		memcpy(data, h->data, moved);
	}
	lock_give(&h->lock);
	return moved;
}

static void pipe_arm(struct pipe *p)
{
	td_fill(p->active, TD_IN | TD_NO_INT | TD_ROUND, p->buf, p->len, p->spare);
	memset(p->spare, 0, sizeof *p->spare);
	__asm__ volatile("sync" : : : "memory");
	p->ed->tail = le(phys(p->spare));
}

void *ohci_int_open(int hc, int addr, int ep, int low, int mps, int len)
{
	struct ohci *h = &hcs[hc];
	uint8_t *page = (uint8_t *)as_page_alloc();
	if (!page)
		return NULL;
	struct pipe *p = (struct pipe *)page;
	p->hc = hc;
	p->ed = (struct ed *)(page + 256);
	p->active = (struct td *)(page + 512);
	p->spare = (struct td *)(page + 528);
	p->buf = page + 1024;
	p->len = len;

	p->ed->flags = le(addr | (ep << 7) | (2 << 11) | (low ? 1 << 13 : 0) | (mps << 16));
	p->ed->head = le(phys(p->active));
	pipe_arm(p);

	uint64_t msr = irq_off();
	p->ed->next = h->periodic->next;
	__asm__ volatile("sync" : : : "memory");
	h->periodic->next = le(phys(p->ed));
	irq_restore(msr);
	return p;
}

int ohci_int_poll(void *pipe, uint8_t *buf)
{
	struct pipe *p = pipe;
	uint32_t head = le(p->ed->head);
	if (head & 1) {
		// keep the toggle carry or every report after this looks like a resend
		klog("usb: interrupt pipe halted, cc %d\n", td_cc(p->active));
		pipe_arm(p);
		p->ed->head = le(phys(p->active) | (head & 2));
		return -1;
	}
	if ((head & ~0xfu) != phys(p->spare))
		return 0;
	int cc = td_cc(p->active);
	int got = 0;
	if (cc == 0 || cc == 9) {
		uint32_t cbp = le(p->active->cbp);
		got = cbp ? (int)(cbp - phys(p->buf)) : p->len;
		memcpy(buf, p->buf, got);
	}
	struct td *t = p->active;
	p->active = p->spare;
	p->spare = t;
	pipe_arm(p);
	return got;
}

void *ohci_bulk_open(int hc, int addr, int ep, int low, int mps)
{
	struct ohci *h = &hcs[hc];
	uint8_t *page = (uint8_t *)as_page_alloc();
	uint8_t *data = (uint8_t *)as_page_alloc();
	if (!page || !data)
		return NULL;
	struct pipe *p = (struct pipe *)page;
	p->hc = hc;
	p->ed = (struct ed *)(page + 256);
	p->active = (struct td *)(page + 512);
	p->spare = (struct td *)(page + 528);
	p->buf = data;
	p->len = PAGE;
	p->ed->flags = le(addr | (ep << 7) | (low ? 1 << 13 : 0) | (mps << 16));
	p->ed->head = le(phys(p->active));
	p->ed->tail = le(phys(p->active));

	uint64_t msr = irq_off();
	p->ed->next = h->bulk->next;
	__asm__ volatile("sync" : : : "memory");
	h->bulk->next = le(phys(p->ed));
	irq_restore(msr);
	return p;
}

// bytes moved or < 0, -4 is a stall
int ohci_bulk(void *pipe, int in, void *buf, int len)
{
	struct pipe *p = pipe;
	struct ohci *h = &hcs[p->hc];
	int moved = 0;
	while (moved < len) {
		int n = len - moved < PAGE ? len - moved : PAGE;
		if (!in)
			memcpy(p->buf, (uint8_t *)buf + moved, n);
		td_fill(p->active, (in ? TD_IN | TD_ROUND : TD_OUT) | TD_NO_INT, p->buf, n, p->spare);
		memset(p->spare, 0, sizeof *p->spare);
		__asm__ volatile("sync" : : : "memory");
		p->ed->tail = le(phys(p->spare));
		wr(h, HC_COMMAND, CMD_BLF);

		uint64_t end = mftb() + platform_tb_freq() * 5;
		uint32_t head;
		while (((head = le(p->ed->head)) & ~0xfu) != phys(p->spare) && !(head & 1) && mftb() < end)
			delay_us(20);
		int cc = td_cc(p->active);
		struct td *t = p->active;
		p->active = p->spare;
		p->spare = t;
		if ((head & 1) || (head & ~0xfu) != phys(p->active) || (cc != 0 && cc != 9))
			return -(cc ? cc : 99);
		int got = n;
		uint32_t cbp = le(t->cbp);
		if (in && cbp)
			got = cbp - phys(p->buf);
		if (in)
			memcpy((uint8_t *)buf + moved, p->buf, got);
		moved += got;
		if (got < n)
			break;
	}
	return moved;
}

// after a stall and CLEAR_FEATURE(ENDPOINT_HALT) the device starts over at DATA0, so does the ed
void ohci_bulk_reset(void *pipe)
{
	struct pipe *p = pipe;
	p->ed->head = le(phys(p->active));
	p->ed->tail = le(phys(p->active));
}

void ohci_int_close(void *pipe)
{
	struct pipe *p = pipe;
	struct ohci *h = &hcs[p->hc];
	p->ed->flags |= le(ED_SKIP);
	uint64_t msr = irq_off();
	for (struct ed *e = h->periodic; e->next;) {
		struct ed *n = (struct ed *)(uint64_t)le(e->next);
		if (n == p->ed) {
			e->next = n->next;
			break;
		}
		e = n;
	}
	irq_restore(msr);
	delay_us(2000);
	as_page_free((uint64_t)pipe);
}
