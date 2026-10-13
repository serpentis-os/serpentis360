#include "kernel.h"
#include "ppc.h"

// address_space.asm again: pages from 16 MB up, freed ones go on a stack, all zeroed

static uint64_t pool_start, pool_end;
static uint64_t as_next_page;
static uint64_t *as_free_stack;
static uint64_t as_free_count, as_free_max;

void mm_init(void)
{
	uint64_t start, end;
	platform_memory(&start, &end);
	pool_start = start > AS_POOL_START ? start : AS_POOL_START;
	pool_end = end < AS_POOL_END ? end : AS_POOL_END;

	as_free_max = (pool_end - pool_start) / PAGE;
	as_free_stack = (uint64_t *)pool_start;
	pool_start += (as_free_max * 8 + PAGE - 1) & ~(uint64_t)(PAGE - 1);
	as_free_count = 0;
	as_next_page = pool_start;
	kprintf("memory: pool %lx-%lx, %ld KB\n", pool_start, pool_end, (long)(pool_end - pool_start) / 1024);
}

uint64_t as_page_alloc(void)
{
	uint64_t msr = irq_off();
	uint64_t page = 0;
	if (as_free_count)
		page = as_free_stack[--as_free_count];
	else if (as_next_page < pool_end) {
		page = as_next_page;
		as_next_page += PAGE;
	}
	irq_restore(msr);
	if (page)
		memset((void *)page, 0, PAGE);
	return page;
}

int as_page_free(uint64_t page)
{
	if (!page || page < pool_start || page >= pool_end || (page & (PAGE - 1)))
		return -1;
	uint64_t msr = irq_off();
	int ok = as_free_count < as_free_max;
	if (ok)
		as_free_stack[as_free_count++] = page;
	irq_restore(msr);
	return ok ? 0 : -1;
}

// more than one page only comes off the top, the free stack has no idea which pages are
// next to each other. thread stacks are the only ones that ask
void *page_alloc(int count)
{
	if (count == 1)
		return (void *)as_page_alloc();
	uint64_t msr = irq_off();
	uint64_t page = 0;
	if (as_next_page + (uint64_t)count * PAGE <= pool_end) {
		page = as_next_page;
		as_next_page += (uint64_t)count * PAGE;
	}
	irq_restore(msr);
	if (page)
		memset((void *)page, 0, (size_t)count * PAGE);
	return (void *)page;
}

void page_free(void *p, int count)
{
	for (int i = 0; i < count; i++)
		if (as_page_free((uint64_t)p + (uint64_t)i * PAGE))
			panic("page_free: %p is not from the pool", p);
}

int pages_free(void)
{
	return (pool_end - as_next_page) / PAGE + as_free_count;
}
