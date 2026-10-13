#include "kernel.h"
#include "ppc.h"
#include "usb.h"

#define MAX_PORTS 16

static struct usb_dev devs[USB_MAX_DEVICES];
static int root_present[4][MAX_PORTS];
int usb_settled;
static const char *kinds[] = {"none", "keyboard", "mouse", "hub", "storage", "other"};

static int control(struct usb_dev *d, int type, int req, int value, int index, void *data, int len)
{
	uint8_t setup[8] = {type, req, value & 0xff, value >> 8, index & 0xff, index >> 8, len & 0xff, len >> 8};
	return ohci_control(d->hc, d->addr, d->low_speed, d->mps0, setup, data, len);
}

static int free_address(void)
{
	for (int a = 1; a < 128; a++) {
		int taken = 0;
		for (int i = 0; i < USB_MAX_DEVICES; i++)
			taken |= devs[i].used && devs[i].addr == a;
		if (!taken)
			return a;
	}
	return 0;
}

// first interface only, with its first in and out endpoint. a keyboard with an extra
// media key interface just loses the media keys
static void classify(struct usb_dev *d, const uint8_t *cfg, int len)
{
	int found = 0;
	for (int at = 0; at + 2 <= len && cfg[at]; at += cfg[at]) {
		const uint8_t *x = cfg + at;
		if (x[1] == 2) {
			d->config = x[5];
		} else if (x[1] == 4) {
			if (found)
				break;
			d->iface = x[2];
			d->class = x[5];
			d->subclass = x[6];
			d->protocol = x[7];
			found = 1;
		} else if (x[1] == 5 && found) {
			int ep = x[2] & 0x0f, in = x[2] & 0x80, type = x[3] & 3;
			int mps = x[4] | (x[5] & 7) << 8;
			if (in && !d->ep_in && (type == 3 || type == 2)) {
				d->ep_in = ep;
				d->ep_in_mps = mps;
			} else if (!in && !d->ep_out && type == 2) {
				d->ep_out = ep;
				d->ep_out_mps = mps;
			}
		}
	}
	if (d->class == 9)
		d->kind = USB_HUB;
	else if (d->class == 3 && d->subclass == 1 && d->protocol == 1)
		d->kind = USB_KEYBOARD;
	else if (d->class == 3 && d->subclass == 1 && d->protocol == 2)
		d->kind = USB_MOUSE;
	else if (d->class == 8)
		d->kind = USB_STORAGE;
	else
		d->kind = USB_OTHER;
}

static void hub_attach(struct usb_dev *d)
{
	uint8_t hd[16];
	if (control(d, 0xa0, 6, 0x2900, 0, hd, sizeof hd) < 7)
		return;
	d->hub_ports = hd[2] < MAX_PORTS ? hd[2] : MAX_PORTS;
	for (int p = 1; p <= d->hub_ports; p++)
		control(d, 0x23, 3, 8, p, NULL, 0);
	sleep_ms(hd[5] * 2 + 20);
}

// address 0 first for the packet size, then a real address, then the whole config
static struct usb_dev *enumerate(int hc, int hub, int port, int low)
{
	struct usb_dev t = {0};
	t.hc = hc;
	t.low_speed = low;
	t.mps0 = 8;
	uint8_t desc[18];
	if (control(&t, 0x80, 6, 0x0100, 0, desc, 8) < 8) {
		klog("usb: device on port %d does not answer\n", port + 1);
		return NULL;
	}
	t.mps0 = desc[7] ? desc[7] : 8;

	struct usb_dev *d = NULL;
	for (int i = 0; i < USB_MAX_DEVICES && !d; i++)
		if (!devs[i].used)
			d = &devs[i];
	int addr = free_address();
	if (!d || !addr || control(&t, 0x00, 5, addr, 0, NULL, 0) < 0)
		return NULL;
	sleep_ms(5);

	t.addr = addr;
	t.hub = hub;
	t.port = port;
	if (control(&t, 0x80, 6, 0x0100, 0, desc, 18) < 18)
		return NULL;
	t.vendor = desc[8] | desc[9] << 8;
	t.product = desc[10] | desc[11] << 8;

	uint8_t cfg[256];
	if (control(&t, 0x80, 6, 0x0200, 0, cfg, 9) < 9)
		return NULL;
	int total = cfg[2] | cfg[3] << 8;
	if (total > (int)sizeof cfg)
		total = sizeof cfg;
	total = control(&t, 0x80, 6, 0x0200, 0, cfg, total);
	if (total < 9)
		return NULL;
	classify(&t, cfg, total);
	if (control(&t, 0x00, 9, t.config, 0, NULL, 0) < 0)
		return NULL;

	*d = t;
	d->used = 1;
	klog("usb: %s %04x:%04x at address %d%s\n", kinds[d->kind], d->vendor, d->product, d->addr,
		d->low_speed ? ", low speed" : "");
	if (d->kind == USB_HUB)
		hub_attach(d);
	else if (d->kind == USB_KEYBOARD || d->kind == USB_MOUSE)
		hid_attach(d);
	else if (d->kind == USB_STORAGE)
		storage_attach(d);
	return d;
}

static void detach(int hc, int hub, int port)
{
	for (int i = 0; i < USB_MAX_DEVICES; i++) {
		struct usb_dev *d = &devs[i];
		if (!d->used || d->hc != hc || d->hub != hub || d->port != port)
			continue;
		if (d->kind == USB_HUB)
			for (int p = 1; p <= d->hub_ports; p++)
				detach(hc, d->addr, p);
		if (d->pipe)
			ohci_int_close(d->pipe);
		if (d->kind == USB_STORAGE)
			storage_detach(d);
		if (d->kind == USB_KEYBOARD)
			hid_detach(d);
		klog("usb: %s at address %d gone\n", kinds[d->kind], d->addr);
		memset(d, 0, sizeof *d);
	}
}

static int attached(int hc, int hub, int port)
{
	for (int i = 0; i < USB_MAX_DEVICES; i++)
		if (devs[i].used && devs[i].hc == hc && devs[i].hub == hub && devs[i].port == port)
			return 1;
	return 0;
}

static void scan_root(int hc)
{
	for (int p = 0; p < ohci_ports(hc) && p < MAX_PORTS; p++) {
		uint32_t s = ohci_port_status(hc, p);
		if (s & (1 << 16))
			ohci_port_write(hc, p, 1 << 16);
		int connected = s & 1;
		if (connected && !root_present[hc][p]) {
			root_present[hc][p] = 1;
			ohci_port_write(hc, p, 1 << 4);
			for (int i = 0; i < 20 && !(ohci_port_status(hc, p) & (1 << 20)); i++)
				sleep_ms(5);
			ohci_port_write(hc, p, 1 << 20);
			sleep_ms(20);
			enumerate(hc, 0, p, (ohci_port_status(hc, p) >> 9) & 1);
		} else if (!connected && root_present[hc][p]) {
			root_present[hc][p] = 0;
			detach(hc, 0, p);
		}
	}
}

static void scan_hub(struct usb_dev *h)
{
	for (int p = 1; p <= h->hub_ports; p++) {
		uint8_t st[4];
		if (control(h, 0xa3, 0, 0, p, st, 4) < 4)
			continue;
		int status = st[0] | st[1] << 8, change = st[2] | st[3] << 8;
		if (change & 1)
			control(h, 0x23, 1, 16, p, NULL, 0);
		int connected = status & 1;
		int known = attached(h->hc, h->addr, p);
		if (connected && !known) {
			control(h, 0x23, 3, 4, p, NULL, 0);
			for (int i = 0; i < 20; i++) {
				sleep_ms(5);
				if (control(h, 0xa3, 0, 0, p, st, 4) == 4 && (st[2] & 0x10))
					break;
			}
			control(h, 0x23, 1, 20, p, NULL, 0);
			sleep_ms(20);
			enumerate(h->hc, h->addr, p, (st[1] >> 1) & 1);
		} else if (!connected && known) {
			detach(h->hc, h->addr, p);
		}
	}
}

void usb_init(void)
{
	uint64_t bases[4];
	int n = platform_usb_ohci(bases, 4);
	for (int i = 0; i < n; i++)
		ohci_init(bases[i]);
	if (!ohci_count())
		klog("usb: no controller\n");
}

// all of usb in one thread. ports every 100 ms, reports every 10. usb_settled tells the
// boot it waited long enough for a stick to show up
void usb_main(void *arg)
{
	for (int round = 0;; round++) {
		if (round % 10 == 0) {
			for (int hc = 0; hc < ohci_count(); hc++)
				scan_root(hc);
			for (int i = 0; i < USB_MAX_DEVICES; i++)
				if (devs[i].used && devs[i].kind == USB_HUB)
					scan_hub(&devs[i]);
		}
		if (round == 20)
			usb_settled = 1;
		for (int i = 0; i < USB_MAX_DEVICES; i++) {
			struct usb_dev *d = &devs[i];
			uint8_t report[8];
			if (!d->used || !d->pipe)
				continue;
			int n = ohci_int_poll(d->pipe, report);
			if (n > 0)
				hid_report(d, report, n);
		}
		hid_repeat();
		sleep_ms(10);
	}
}

void usb_list(void)
{
	int any = 0;
	for (int i = 0; i < USB_MAX_DEVICES; i++) {
		struct usb_dev *d = &devs[i];
		if (!d->used)
			continue;
		any = 1;
		kprintf("Bus %d Device %03d: ID %04x:%04x %-8s %s port %d%s\n", d->hc, d->addr, d->vendor,
			d->product, kinds[d->kind], d->hub ? "hub" : "root", d->hub ? d->port : d->port + 1,
			d->low_speed ? " (low speed)" : "");
	}
	if (!any)
		kprintf("No usb devices.\n");
}
