#ifndef KERNEL_H
#define KERNEL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

#define HZ 100
#define PAGE 4096
// same numbers as serpentis-os. on the 360 the kernel and whatever xell left sit below 16 MB
#define AS_POOL_START 0x01000000ull
#define AS_POOL_END 0x1e000000ull

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strncpy(char *d, const char *s, size_t n);
char *strcpy(char *d, const char *s);
char *strcat(char *d, const char *s);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);

/* platform, one of platform/qemu, mac or xenon gets linked in */
void platform_init(uint64_t fdt, uint64_t r8, uint64_t r9);
void platform_putc(char c);
int platform_getc(void);
uint32_t platform_pvr(void);
void platform_flush(void);
int platform_usb_ohci(uint64_t *bases, int max);
uint64_t platform_tb_freq(void);
void platform_memory(uint64_t *start, uint64_t *end);
const char *platform_name(void);
void platform_halt(void) __attribute__((noreturn));
void platform_reboot(void) __attribute__((noreturn));
void platform_poweroff(void) __attribute__((noreturn));

void mm_init(void);
uint64_t as_page_alloc(void);
int as_page_free(uint64_t page);
void *page_alloc(int count);
void page_free(void *p, int count);
int pages_free(void);

extern struct cpu cpu0;
void trap_init(void);
void dump_frame(const struct frame *f);
struct frame *trap(struct frame *f);

extern volatile uint64_t ticks;
void timer_init(void);
void timer_tick(void);

enum { T_FREE, T_READY, T_SLEEPING, T_DEAD };
struct thread {
	struct frame *frame;
	void *stack;
	int state;
	int id;
	uint64_t wake;
	uint64_t runs;
	char name[16];
};
void sched_init(void);
struct thread *thread_create(void (*fn)(void *), void *arg, const char *name);
void thread_exit(void) __attribute__((noreturn));
void yield(void);
void lock_take(volatile int *lock);
void lock_give(volatile int *lock);
void sleep_ms(uint64_t ms);
struct frame *schedule(struct frame *f);
void sched_wake(void);
int threads_alive(void);
void threads_list(void);

void shell_main(void *arg);

#endif
