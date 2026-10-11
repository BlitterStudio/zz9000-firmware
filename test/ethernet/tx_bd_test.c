/* SPDX-License-Identifier: MIT
 * Host fixture for the firmware TX completion handler on the bundled BSP
 * ring code: several asynchronous frames in flight, completed out of the
 * handler's sight one at a time.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef uintptr_t UINTPTR;
typedef long LONG;
typedef u32 XEmacPs_Bd[2];
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define XST_DMA_SG_LIST_ERROR 2
#define XEMACPS_TXSR_OFFSET 0u
/* BSP_DEFINITIONS */
#include "eth_tx_order.h"
typedef struct { struct { u32 BaseAddress; } Config; XEmacPs_BdRing TxBdRing; } XEmacPs;
static XEmacPs EmacPsInstance;
static XEmacPs_Bd descriptors[TXBD_CNT];
static struct eth_tx_order eth_tx_ord;
static u32 FramesTx;
#define XEmacPs_GetTxRing(instance) ((instance)->TxBdRing)
#define XEmacPs_BdRead(bd, offset) (*(u32 *)((u8 *)(bd) + (offset)))
#define XEmacPs_BdWrite(bd, offset, value) (*(u32 *)((u8 *)(bd) + (offset)) = (u32)(value))
static u32 XEmacPs_ReadReg(u32 base, u32 offset) { (void)base; (void)offset; return 0; }
static void XEmacPs_WriteReg(u32 base, u32 offset, u32 value) { (void)base; (void)offset; (void)value; }
/* BSP_FUNCTIONS */
/* TX_FUNCTIONS */
static void setup(void)
{
    XEmacPs_BdRing *r = &EmacPsInstance.TxBdRing;
    memset(&EmacPsInstance, 0, sizeof(EmacPsInstance));
    memset(descriptors, 0, sizeof(descriptors));
    memset(&eth_tx_ord, 0, sizeof(eth_tx_ord));
    r->BaseBdAddr = r->PhysBaseAddr = (UINTPTR)&descriptors[0];
    r->HighBdAddr = (UINTPTR)&descriptors[TXBD_CNT - 1];
    r->Separation = sizeof(descriptors[0]); r->Length = sizeof(descriptors);
    r->FreeHead = r->PreHead = r->HwHead = r->HwTail = r->PostHead = &descriptors[0];
    r->AllCnt = r->FreeCnt = TXBD_CNT;
    for (unsigned i = 0; i < TXBD_CNT; i++)
        descriptors[i][1] = XEMACPS_TXBUF_USED_MASK | (i == TXBD_CNT - 1 ? XEMACPS_TXBUF_WRAP_MASK : 0);
}
/* What ethernet_send_frame_async() does with a BD, minus the GEM. */
static XEmacPs_Bd *submit(void)
{
    XEmacPs_BdRing *r = &EmacPsInstance.TxBdRing;
    XEmacPs_Bd *bd;
    assert(XEmacPs_BdRingAlloc(r, 1, &bd) == XST_SUCCESS);
    (*bd)[1] = ((*bd)[1] & XEMACPS_TXBUF_WRAP_MASK) | XEMACPS_TXBUF_LAST_MASK | 1514u; /* USED clear: GEM owns it */
    assert(XEmacPs_BdRingToHw(r, 1, bd) == XST_SUCCESS);
    eth_tx_order_push(&eth_tx_ord, 1);
    return bd;
}
static void gem_done(XEmacPs_Bd *bd) { (*bd)[1] |= XEMACPS_TXBUF_USED_MASK; }
int main(void)
{
    XEmacPs_BdRing *r = &EmacPsInstance.TxBdRing;
    setup();
    /* One lap first: a BD freed after a frame keeps USED and its LAST bit,
     * which is what lets the BSP's scan mistake it for a completed frame. */
    for (unsigned i = 0; i < TXBD_CNT; i++) {
        gem_done(submit());
        XEmacPsSendHandler(&EmacPsInstance);
    }
    assert(eth_tx_ord.done == TXBD_CNT && !r->HwCnt && r->FreeCnt == TXBD_CNT);
    memset(&eth_tx_ord, 0, sizeof(eth_tx_ord)); FramesTx = 0;
    XEmacPs_Bd *a = submit(), *b = submit(), *c = submit();
    (void)c;
    gem_done(a);
    XEmacPsSendHandler(&EmacPsInstance);
    /* Only A is done: B and C are still in DMA and must stay outstanding. */
    assert(eth_tx_ord.done == 1 && FramesTx == 1);
    assert(r->HwCnt == 2 && r->HwHead == b && r->FreeCnt == TXBD_CNT - 2);
    XEmacPsSendHandler(&EmacPsInstance);	/* a spurious pass changes nothing */
    assert(eth_tx_ord.done == 1 && r->HwCnt == 2);
    gem_done(b); gem_done(c);
    XEmacPsSendHandler(&EmacPsInstance);
    assert(eth_tx_ord.done == 3 && FramesTx == 3 && !r->HwCnt && r->FreeCnt == TXBD_CNT);
    puts("PASS tx: only BDs the GEM has finished retire, in order, with frames still in flight");
    return 0;
}
