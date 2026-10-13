ENTRY(_start)

SECTIONS
{
	. = KERNEL_BASE;
	__image_start = .;
	.text : {
		*(.text.entry)
		*(.text.head)
		*(.text .text.*)
	}
	.rodata : { *(.rodata .rodata.*) }
	.opd : { *(.opd) }
	.data : { *(.data .data.*) *(.sdata .sdata.*) }
	.got : { *(.got) *(.toc) }
	. = ALIGN(16);
	__image_end = .;
	__bss_start = .;
	.bss : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
	. = ALIGN(16);
	__bss_end = .;
	__kernel_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
