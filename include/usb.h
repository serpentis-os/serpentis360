#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_MAX_DEVICES 16

enum { USB_NONE, USB_KEYBOARD, USB_MOUSE, USB_HUB, USB_STORAGE, USB_OTHER };

struct usb_dev {
	int used;
	int addr;
	int low_speed;
	int mps0;
	int hub;		// address of the hub it hangs off, 0 = root port
	int port;
	int hc;
	int kind;
	int iface;
	int config;
	int ep_in, ep_in_mps, ep_out, ep_out_mps;
	uint16_t vendor, product;
	uint8_t class, subclass, protocol;
	int hub_ports;
	void *pipe;
	// sticks only
	void *bulk_in, *bulk_out;
	int disk;
	uint8_t last[8];
};

int ohci_init(uint64_t base);
int ohci_count(void);
int ohci_ports(int hc);
uint32_t ohci_port_status(int hc, int port);
void ohci_port_write(int hc, int port, uint32_t value);
int ohci_control(int hc, int addr, int low, int mps, const uint8_t setup[8], void *data, int len);
void *ohci_int_open(int hc, int addr, int ep, int low, int mps, int len);
int ohci_int_poll(void *pipe, uint8_t *buf);
void ohci_int_close(void *pipe);
void *ohci_bulk_open(int hc, int addr, int ep, int low, int mps);
int ohci_bulk(void *pipe, int in, void *buf, int len);
void ohci_bulk_reset(void *pipe);

void usb_init(void);
void usb_main(void *arg);
void usb_list(void);

void storage_attach(struct usb_dev *d);
void storage_detach(struct usb_dev *d);

void hid_attach(struct usb_dev *d);
void hid_report(struct usb_dev *d, const uint8_t *report, int len);
void hid_repeat(void);
void hid_detach(struct usb_dev *d);
extern int mouse_x, mouse_y, mouse_buttons;

void input_push(int c);
void input_push_str(const char *s);
int input_getc(void);

#endif
