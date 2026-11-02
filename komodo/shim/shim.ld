ENTRY(_start)
SECTIONS
{
	. = 0;
	.head : { KEEP(*(.head.text)) }
	.text : { *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. = ALIGN(16);
	. = . + 0x8000;        /* 32 KiB стек */
	_stack_top = .;
	. = ALIGN(8);
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
