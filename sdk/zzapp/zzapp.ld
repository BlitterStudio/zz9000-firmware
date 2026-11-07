/*
 * PIE app image. Vaddrs are offsets from 0; the loader adds the arena base.
 * GNU ld's allocated .rel.dyn is loaded directly; SHT_REL and SHT_RELA are
 * both part of the public loader contract.
 *
 * SPDX-License-Identifier: MIT
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(zz_start)

PHDRS {
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
	note PT_NOTE FLAGS(4);
	dyn  PT_DYNAMIC FLAGS(4);
}

SECTIONS {
	. = 0;

	.text : {
		*(.text.zz_start)
		*(.text*)
		*(.text.*)
	} :text

	.rodata : {
		*(.rodata*)
		*(.rodata.*)
	} :text


	/*
	 * Keep either GNU ld's .rel.dyn or a toolchain's .rela.dyn so
	 * --gc-sections cannot drop the loader-visible relocation records.
	 */
	.rel.dyn : {
		KEEP(*(.rel.dyn))
		KEEP(*(.rel.plt))
		KEEP(*(.rela.dyn))
		KEEP(*(.rela.plt))
	} :text

	.dynamic : { *(.dynamic) } :text :dyn
	.got : { *(.got) *(.got.plt) } :text
	.hash : { *(.hash) } :text
	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text

	.note.zzapp : {
		KEEP(*(.note.zzapp))
	} :text :note

	.data : {
		*(.data*)
		*(.data.*)
	} :data

	.bss : {
		*(.bss*)
		*(.bss.*)
		*(COMMON)
	} :data

	/DISCARD/ : {
		*(.interp)
		*(.comment)
		/* Apps disable unwinding; unwinder archives carry absolute relocs. */
		*(.ARM.exidx*)
		*(.ARM.extab*)
		*(.eh_frame*)
		*(.note.GNU-stack)
	}
}
