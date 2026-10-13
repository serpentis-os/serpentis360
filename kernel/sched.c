#include "kernel.h"
#include "ppc.h"

// round robin, slot 0 ends up as idle. switches only happen in trap()

#define THREADS 32
#define STACK_PAGES 4

static struct thread threads[THREADS];
static struct thread *current;
static uint64_t kernel_msr;
static int next_id;

void sched_init(void)
{
	current = &threads[0];
	current->state = T_READY;
	strncpy(current->name, "idle", sizeof current->name - 1);
	cpu0.current = current;
	kernel_msr = mfmsr() | MSR_EE | MSR_RI;
	next_id = 1;
}

struct thread *thread_create(void (*fn)(void *), void *arg, const char *name)
{
	uint64_t msr = irq_off();
	struct thread *t = NULL;
	for (int i = 1; i < THREADS && !t; i++) {
		if (threads[i].state == T_DEAD) {
			page_free(threads[i].stack, STACK_PAGES);
			threads[i].state = T_FREE;
		}
		if (threads[i].state == T_FREE)
			t = &threads[i];
	}
	irq_restore(msr);
	if (!t)
		return NULL;

	memset(t, 0, sizeof *t);
	t->stack = page_alloc(STACK_PAGES);
	if (!t->stack)
		return NULL;
	uint64_t top = (uint64_t)t->stack + STACK_PAGES * PAGE;
	struct frame *f = (struct frame *)(top - 1024);
	const uint64_t *desc = (const uint64_t *)fn;
	const uint64_t *exit_desc = (const uint64_t *)thread_exit;

	f->srr0 = desc[0];
	f->srr1 = kernel_msr;
	f->gpr[1] = top - ABI_FRAME;
	f->gpr[2] = desc[1];
	f->gpr[3] = (uint64_t)arg;
	f->gpr[13] = (uint64_t)&cpu0;
	f->lr = exit_desc[0];
	*(uint64_t *)(top - ABI_FRAME) = 0;

	t->frame = f;
	t->id = next_id++;
	strncpy(t->name, name, sizeof t->name - 1);
	t->state = T_READY;
	return t;
}

// still on the dead stack here, it gets freed when the slot is reused
void thread_exit(void)
{
	irq_off();
	current->state = T_DEAD;
	yield();
	panic("dead thread %s came back", current->name);
}

void yield(void)
{
	__asm__ volatile("sc" : : : "memory");
}

// one cpu thread, so interrupts off around the test is the whole lock
void lock_take(volatile int *lock)
{
	for (;;) {
		uint64_t msr = irq_off();
		if (!*lock) {
			*lock = 1;
			irq_restore(msr);
			return;
		}
		irq_restore(msr);
		yield();
	}
}

void lock_give(volatile int *lock)
{
	*lock = 0;
}

// rounds up to whole ticks, sleep_ms(1) is 10 ms
void sleep_ms(uint64_t ms)
{
	uint64_t msr = irq_off();
	current->wake = ticks + (ms * HZ + 999) / 1000;
	current->state = T_SLEEPING;
	irq_restore(msr);
	yield();
}

void sched_wake(void)
{
	for (int i = 1; i < THREADS; i++)
		if (threads[i].state == T_SLEEPING && ticks >= threads[i].wake)
			threads[i].state = T_READY;
}

struct frame *schedule(struct frame *f)
{
	current->frame = f;
	struct thread *next = &threads[0];
	int at = current - threads;
	for (int i = 1; i <= THREADS; i++) {
		struct thread *t = &threads[(at + i) % THREADS];
		if (t != &threads[0] && t->state == T_READY) {
			next = t;
			break;
		}
	}
	current = next;
	cpu0.current = next;
	next->runs++;
	return next->frame;
}

void threads_list(void)
{
	static const char *states[] = {"free", "ready", "sleeping", "dead"};
	kprintf("  ID  NAME          STATE      RUNS\n");
	for (int i = 0; i < THREADS; i++) {
		struct thread *t = &threads[i];
		if (t->state == T_FREE)
			continue;
		kprintf("%4d  %-12s  %-9s %ld%s\n", t->id, t->name, states[t->state], (long)t->runs,
			t == current ? "  <- this one" : "");
	}
}

int threads_alive(void)
{
	int n = 0;
	for (int i = 1; i < THREADS; i++)
		n += threads[i].state == T_READY || threads[i].state == T_SLEEPING;
	return n;
}
