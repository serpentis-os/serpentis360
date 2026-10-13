#include "asm.h"

// long opal_call(long token, long a, long b, long c)
// opal wants the token in r0, its own base in r2, and may trash r13
	.section .text
FUNC opal_call
	mflr	%r0
	std	%r0, 16(%r1)
	std	%r2, 40(%r1)
	stdu	%r1, -128(%r1)
	std	%r13, 112(%r1)
	mr	%r0, %r3
	mr	%r3, %r4
	mr	%r4, %r5
	mr	%r5, %r6
	LOADADDR %r11, opal_base
	ld	%r2, 0(%r11)
	LOADADDR %r11, opal_entry
	ld	%r11, 0(%r11)
	mtctr	%r11
	bctrl
	ld	%r13, 112(%r1)
	addi	%r1, %r1, 128
	ld	%r2, 40(%r1)
	ld	%r0, 16(%r1)
	mtlr	%r0
	blr
