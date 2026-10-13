#include "kernel.h"
#include "ppc.h"

// runs on qemu at every build, the output gets grepped by test.sh

static volatile int steps[3];
static volatile int preempted;

static int total(void)
{
	return steps[0] + steps[1] + steps[2];
}

static void worker(void *arg)
{
	int n = (long)arg;
	for (int i = 0; i < 5; i++) {
		steps[n]++;
		kprintf("worker %d step %d at tick %ld\n", n, i, (long)ticks);
		sleep_ms(40 * (n + 1));
	}
}

// never yields. if the others still move while this spins, the timer really preempts
static void spinner(void *arg)
{
	int before = total();
	uint64_t end = ticks + 30;
	while (ticks < end)
		;
	preempted = total() > before;
}

static void check(void *arg)
{
	int ok = 1;
	while (threads_alive() > 2)
		sleep_ms(20);

	for (int i = 0; i < 3; i++)
		if (steps[i] != 5) {
			kprintf("FAIL worker %d did %d steps\n", i, steps[i]);
			ok = 0;
		}
	if (!preempted) {
		kprintf("FAIL spinner was never preempted\n");
		ok = 0;
	}

	int before = pages_free();
	void *a = page_alloc(3), *b = page_alloc(1), *c = page_alloc(8);
	if (!a || !b || !c || a == b || pages_free() != before - 12) {
		kprintf("FAIL page_alloc\n");
		ok = 0;
	}
	page_free(b, 1);
	void *d = page_alloc(1);
	if (d != b) {
		kprintf("FAIL page_alloc did not reuse a freed page\n");
		ok = 0;
	}
	page_free(a, 3);
	page_free(c, 8);
	page_free(d, 1);
	if (pages_free() != before) {
		kprintf("FAIL pages leaked\n");
		ok = 0;
	}

	char buf[64];
	snprintf(buf, sizeof buf, "%d %u %x %5s|%-4d|%04x %ld %c", -42, 7u, 0xbeef, "ab", 3, 0x2a, -1l, 'z');
	if (strcmp(buf, "-42 7 beef    ab|3   |002a -1 z")) {
		kprintf("FAIL printf gave '%s'\n", buf);
		ok = 0;
	}

	kprintf("selftest %s after %ld ticks\n", ok ? "ok" : "FAILED", (long)ticks);
	kprintf("now an illegal instruction on purpose\n");
	__asm__ volatile(".long 0");
}

void selftest(void)
{
	for (long i = 0; i < 3; i++)
		thread_create(worker, (void *)i, "worker");
	thread_create(spinner, NULL, "spinner");
	thread_create(check, NULL, "check");
}
