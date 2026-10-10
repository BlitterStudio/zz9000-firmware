/* SPDX-License-Identifier: MIT
 * Actual restart body; GEM/GIC and init are explicit boundary stubs.
 * Ready-state caller with GEM IRQ enabled; no physical pending IRQ/DMA model.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef struct { struct { u32 BaseAddress; } Config; } XEmacPs;
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define XEMACPS_TXSR_OFFSET 1u
#define XEMACPS_RXSR_OFFSET 2u
#define XEMACPS_ISR_OFFSET 3u
static XEmacPs EmacPsInstance;
static int ethernet_hw_ready, irq_enabled, init_status;
static unsigned pauses, stops, clears, inits, resumes, reg_reads, reg_writes;
static void ethernet_log_status(const char *reason)
{
    assert(reason != NULL);
    if (!strcmp(reason, "dma-restart-after"))
        assert(ethernet_hw_ready && inits == 1 && !irq_enabled);
}
static int ethernet_pause_rx_irq(void)
{ assert(irq_enabled); irq_enabled = 0; pauses++; return 1; }
static void ethernet_resume_rx_irq(int paused)
{
    assert(paused == 1 && !irq_enabled && inits == 1);
    assert(ethernet_hw_ready == (init_status == XST_SUCCESS));
    irq_enabled = 1; resumes++;
}
static void XEmacPs_Stop(XEmacPs *p)
{ assert(p == &EmacPsInstance && !irq_enabled && !ethernet_hw_ready); stops++; }
static u32 XEmacPs_ReadReg(u32 base, unsigned offset)
{ assert(base && offset >= 1 && offset <= 3 && !irq_enabled && stops == 1); reg_reads++; return offset + 16; }
static void XEmacPs_WriteReg(u32 base, unsigned offset, u32 value)
{ assert(base && value == offset + 16 && !irq_enabled && !ethernet_hw_ready); reg_writes++; }
static void ethernet_clear_host_state(void)
{ assert(!irq_enabled && !ethernet_hw_ready && stops == 1); clears++; }
static int init_ethernet_buffers(void)
{ assert(!irq_enabled && !ethernet_hw_ready && clears == 1); inits++; return init_status; }
/* RESTART_FUNCTION */
int main(int argc, char **argv)
{
    assert(argc == 2);
    /* References keep the unfixed-source negative control warning-clean. */
    (void)ethernet_pause_rx_irq; (void)ethernet_resume_rx_irq;
    EmacPsInstance.Config.BaseAddress = !strcmp(argv[1], "no-base") ? 0 : 0x1000u;
    init_status = !strcmp(argv[1], "failure") ? XST_FAILURE : XST_SUCCESS;
    ethernet_hw_ready = irq_enabled = 1;
    assert(ethernet_restart_dma("test") == init_status);
    assert(pauses == 1 && stops == 1 && clears == 1 && inits == 1 && resumes == 1);
    assert(irq_enabled && ethernet_hw_ready == (init_status == XST_SUCCESS));
    assert(reg_reads == (EmacPsInstance.Config.BaseAddress ? 3u : 0u) && reg_reads == reg_writes);
    printf("PASS restart %s\n", argv[1]);
    return 0;
}
