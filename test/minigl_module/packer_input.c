/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A relocation-free ELF32 fixture for exercising the production v3 packer.
 * The host executable dispatches to its linked module entry after validation;
 * this image verifies the exact header the firmware parser receives. */
void zzm_module_entry(void) {}
void zzm_module_exit(void) {}
