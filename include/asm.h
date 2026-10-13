#ifndef ASM_H
#define ASM_H

// the kernel runs at its link address with the mmu off, so a full 64 bit immediate is
// all it takes to reach any symbol
.macro LOADADDR reg, sym
	lis	\reg, \sym@highest
	ori	\reg, \reg, \sym@higher
	rldicr	\reg, \reg, 32, 31
	oris	\reg, \reg, \sym@h
	ori	\reg, \reg, \sym@l
.endm

.macro CALL fn
	LOADADDR %r12, \fn
	ld	%r0, 0(%r12)
	ld	%r2, 8(%r12)
	mtctr	%r0
	bctrl
.endm

.macro FUNC name
	.section ".opd", "aw"
	.balign	8
	.globl	\name
\name:
	.quad	.L.\name, .TOC.@tocbase, 0
	.previous
	.type	\name, @function
.L.\name:
.endm

#endif
