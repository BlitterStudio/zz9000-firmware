/* Link one external module with -Wl,--defsym,ZZM_SLOT=<manifest address>.
 * The packer rejects relocations and any allocated section outside this slot. */
ENTRY(zzm_module_init)
MEMORY { module (rwx) : ORIGIN = ZZM_SLOT, LENGTH = ZZM_SLOT_SIZE }
SECTIONS {
  .text : { *(.text .text.*) *(.rodata .rodata.*) } > module
  .data : { *(.data .data.*) *(.got .got.*) *(.got.plt) } > module
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > module
  /DISCARD/ : { *(.rel.*) *(.rela.*) *(.plt) }
}
ASSERT(SIZEOF(.text) + SIZEOF(.data) + SIZEOF(.bss) <= LENGTH(module),
       "module exceeds fixed slot")
