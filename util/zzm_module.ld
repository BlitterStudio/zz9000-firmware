/* Link one external module with -Wl,--defsym,ZZM_SLOT=<manifest address>.
 * The packer rejects relocations and any allocated section outside this slot. */
ENTRY(zzm_module_entry)
MEMORY { module (rwx) : ORIGIN = ZZM_SLOT, LENGTH = ZZM_SLOT_SIZE }
SECTIONS {
  .text : {
    *(.text .text.*) *(.rodata .rodata.*)
    __zzm_preinit_array_start = .; KEEP(*(.preinit_array)) __zzm_preinit_array_end = .;
    __zzm_init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) __zzm_init_array_end = .;
    __zzm_fini_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) __zzm_fini_array_end = .;
  } > module
  .data : { *(.data .data.*) *(.got .got.*) *(.got.plt) } > module
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > module
  /DISCARD/ : { *(.rel.*) *(.rela.*) *(.plt) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(SIZEOF(.text) + SIZEOF(.data) + SIZEOF(.bss) <= LENGTH(module),
       "module exceeds fixed slot")
