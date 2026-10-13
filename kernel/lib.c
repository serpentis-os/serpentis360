#include "kernel.h"
#include "ppc.h"
#include "screen.h"

// the bits of libc the kernel uses. byte loops, nothing in here is hot enough to care
void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;
	while (n--)
		*a++ = *b++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;
	if (a < b)
		while (n--)
			*a++ = *b++;
	else
		while (n--)
			a[n] = b[n];
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

char *strcat(char *d, const char *s)
{
	strcpy(d + strlen(d), s);
	return d;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (!*s)
			return (char *)last;
	}
}

char *strstr(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!strncmp(hay, needle, n))
			return (char *)hay;
	return n ? NULL : (char *)hay;
}

char *strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}

struct out {
	char *buf;
	size_t size, len;
};

static void emit(struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void number(struct out *o, uint64_t v, int base, int upper, int width, int zero, int left, int neg)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0;
	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	int len = n + neg;
	if (neg && zero)
		emit(o, '-');
	if (!left)
		for (; len < width; width--)
			emit(o, zero ? '0' : ' ');
	if (neg && !zero)
		emit(o, '-');
	while (n)
		emit(o, tmp[--n]);
	if (left)
		for (; len < width; width--)
			emit(o, ' ');
}

// enough printf for a kernel: d i u x X p c s %, width, 0 and -, l and ll
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = {buf, size, 0};
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			emit(&o, *fmt);
			continue;
		}
		fmt++;
		int zero = 0, left = 0, width = 0, longs = 0;
		for (;; fmt++) {
			if (*fmt == '0')
				zero = 1;
			else if (*fmt == '-')
				left = 1;
			else
				break;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + *fmt++ - '0';
		while (*fmt == 'l')
			longs++, fmt++;
		switch (*fmt) {
		case 'd':
		case 'i': {
			int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
			number(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, 0, width, zero, left, v < 0);
			break;
		}
		case 'u':
			number(&o, longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, 0, width, zero, left, 0);
			break;
		case 'x':
		case 'X':
			number(&o, longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, *fmt == 'X', width, zero, left, 0);
			break;
		case 'o':
			number(&o, longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 8, 0, width, zero, left, 0);
			break;
		case 'p':
			emit(&o, '0');
			emit(&o, 'x');
			number(&o, (uint64_t)va_arg(ap, void *), 16, 0, 16, 1, 0, 0);
			break;
		case 'c':
			emit(&o, va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			int len = strlen(s);
			if (!left)
				for (; len < width; width--)
					emit(&o, ' ');
			while (*s)
				emit(&o, *s++);
			for (; len < width; width--)
				emit(&o, ' ');
			break;
		}
		case '%':
			emit(&o, '%');
			break;
		default:
			emit(&o, '%');
			emit(&o, *fmt);
			if (!*fmt)
				fmt--;
		}
	}
	if (o.size)
		o.buf[o.len < o.size ? o.len : o.size - 1] = 0;
	return o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

void kprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	uint64_t msr = irq_off();
	for (char *p = buf; *p; p++) {
		if (*p == '\n')
			platform_putc('\r');
		platform_putc(*p);
		terminal_putc(*p);
	}
	irq_restore(msr);
}

// serial only. usb messages used to land in the middle of the prompt
void klog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	uint64_t msr = irq_off();
	for (char *p = buf; *p; p++) {
		if (*p == '\n')
			platform_putc('\r');
		platform_putc(*p);
	}
	irq_restore(msr);
}

// flush first, on the 360 the tv is the only place a panic will ever be seen
void panic(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	irq_off();
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	kprintf("\npanic: %s\n", buf);
	platform_flush();
	platform_halt();
}
