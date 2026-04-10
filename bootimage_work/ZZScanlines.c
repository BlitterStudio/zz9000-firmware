/*
 * ZZScanlines - Set scanline intensity for ZZ9000 scandoubler
 * 
 * Usage: ZZScanlines <0-255>
 *   0   = scanlines off
 *   128 = 50% (recommended)
 *   255 = max (black lines)
 *
 * Build with m68k-amigaos-gcc:
 *   m68k-amigaos-gcc -O2 -noixemul -o ZZScanlines ZZScanlines.c -lexpath
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>

#define MNT_MANUFACTURER 0x6d6e
#define ZZ9000_PRODUCT_Z3 4
#define ZZ9000_PRODUCT_Z2 3

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <0-255>\n", argv[0]);
        printf("  0   = scanlines off\n");
        printf("  128 = 50%% (recommended)\n");
        printf("  255 = max (black lines)\n");
        return 1;
    }

    int intensity = atoi(argv[1]);
    if (intensity < 0 || intensity > 255) {
        printf("ERROR: Value must be 0-255, got %d\n", intensity);
        return 1;
    }

    struct ExpansionBase *ExpansionBase = NULL;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!ExpansionBase) {
        printf("ERROR: Cannot open expansion.library\n");
        return 1;
    }

    struct ConfigDev *cd = NULL;
    ULONG board_addr = 0;

    while ((cd = FindConfigDev(cd, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z3))) {
        board_addr = (ULONG)cd->cd_BoardAddr;
        break;
    }
    if (!board_addr) {
        while ((cd = FindConfigDev(cd, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z2))) {
            board_addr = (ULONG)cd->cd_BoardAddr;
            break;
        }
    }

    if (!board_addr) {
        printf("ERROR: ZZ9000 not found\n");
        CloseLibrary((struct Library *)ExpansionBase);
        return 1;
    }

    volatile uint16_t *regbase = (volatile uint16_t *)board_addr;

    printf("ZZ9000 found at 0x%08lx\n", board_addr);
    printf("HW version: %d\n", regbase[0x00 / 2]);
    printf("FW version: %d.%d\n", regbase[0xC0 / 2] >> 8, regbase[0xC0 / 2] & 0xFF);

    /*
     * The video control registers in mntzorro.v are decoded at offsets
     * 0x00-0x0a (masked with & 0xff). The driver accesses them at
     * RegisterBase + 0x1000+ to avoid the blitter register space.
     *
     * Register layout in mntzorro.v REGWRITE (regwrite_addr & 'hff):
     *   'h00: video_control_data[31:16]
     *   'h02: video_control_data[15:0]
     *   'h04: video_control_op
     *   'h06: videocap_mode
     *   'h0a: scanline_intensity  (our new register)
     *
     * We write to RegisterBase + 0x100A to reach 'h0a via the mask.
     * Using 16-bit write (same as driver's zzwrite16).
     *
     * On Zorro III, a 16-bit write to even address 0x100A:
     *   - z3addr[15:0] = 0x100A
     *   - ds3 (upper data strobe) fires
     *   - z3addr_regpart = 0x100A
     *   - regwrite_addr & 'hff = 0x0A
     *   - matches 'h0a case
     */
    volatile uint16_t *scanline_reg = (volatile uint16_t *)(board_addr + 0x100A);
    *scanline_reg = (uint16_t)intensity;

    printf("ZZ9000 scanlines set to %d\n", intensity);
    printf("(wrote 0x%04x to register at 0x%08lx)\n", intensity, (ULONG)scanline_reg);

    CloseLibrary((struct Library *)ExpansionBase);
    return 0;
}
