/* SPDX-License-Identifier: MIT
 * Actual init/prepare/clear and BSP ring bodies. Hardware is stubbed.
 * The caller clears host state before init, matching the existing contract.
 * Failure injection reaches real BSP paths; physical DMA/cache are not modeled.
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
#define XST_INVALID_PARAM 2
#define XST_DMA_SG_LIST_ERROR 3
#define XST_DMA_SG_NO_LIST 4
#define XST_DEVICE_IS_STARTED 5
#define XST_DMA_SG_IS_STOPPED 6
#define XST_DMA_SG_IS_STARTED 7
#define Xil_AssertNonvoid(x) assert(x)
#define FRAME_MAX_BACKLOG 128
#define FRAME_SIZE 2048
#define RX_FRAME_PAD 4
#define ETH_INVALID_BACKLOG_SLOT 0xffffu
#define ETH_BACKLOG_HIGH_WATERMARK 120
#define MNTZ_BASE_ADDR 0u
#define MNTZORRO_REG4 4u
/* BSP_DEFINITIONS */
typedef struct { XEmacPs_BdRing RxRing, TxRing; } XEmacPs;
static XEmacPs EmacPsInstance;
static XEmacPs_Bd rx[RXBD_CNT], tx[TXBD_CNT];
#define RX_BD_LIST_START_ADDRESS ((UINTPTR)rx)
#define TX_BD_LIST_START_ADDRESS ((UINTPTR)tx)
#define XEmacPs_GetRxRing(p) ((p)->RxRing)
#define XEmacPs_GetTxRing(p) ((p)->TxRing)
#define XEMACPS_BD_TO_INDEX(r, bd) ((u32)(((UINTPTR)(bd) - (r)->BaseBdAddr) / (r)->Separation))
static int frames_dropped, frames_backlog_full, rx_backpressure, rx_pause_frames, rx_slot_mismatch;
static u16 frames_backlog, frames_backlog_read, frames_backlog_write;
static u16 frames_backlog_reserved, frames_backlog_reserve, frame_serial;
static u16 rx_bd_backlog_slot[RXBD_CNT];
static u32 frames_received, FramesRx, FramesTx;
static unsigned stopped, running, clears, creates, clones, allocs;
static unsigned prepared, commits, barriers, grants, starts;
static const char *failure;
static unsigned prepare_failure;

static void bd_write(XEmacPs_Bd *bd, unsigned offset, UINTPTR value)
{
    UINTPTR p = (UINTPTR)bd;
    if (p >= (UINTPTR)rx && p < (UINTPTR)(rx + RXBD_CNT) && offset == XEMACPS_BD_ADDR_OFFSET &&
        !(value & XEMACPS_RXBUF_NEW_MASK)) {
        unsigned i = XEMACPS_BD_TO_INDEX(&EmacPsInstance.RxRing, bd);
        unsigned slot = rx_bd_backlog_slot[i];
        assert(!running && commits == 1 && barriers == 1);
        assert(EmacPsInstance.RxRing.HwCnt == RXBD_CNT && frames_backlog_reserved == RXBD_CNT);
        assert(slot == i && (value & XEMACPS_RXBUF_ADD_MASK) == 0x100000u + slot * FRAME_SIZE + RX_FRAME_PAD);
        grants++;
    }
    *(u32 *)((u8 *)bd + offset) = (u32)value;
}
#define XEmacPs_BdRead(bd, offset) (*(u32 *)((u8 *)(bd) + (offset)))
#define XEmacPs_BdWrite(bd, offset, value) bd_write((bd), (offset), (value))
/* BSP_FUNCTIONS */
static int failing(const char *stage) { return failure && !strcmp(failure, stage); }
static void XEmacPs_Stop(XEmacPs *p) { assert(p == &EmacPsInstance); running = 0; stopped++; }
static void ethernet_clear_backlog_slot(u16 slot)
{ assert(slot < FRAME_MAX_BACKLOG && !running); }
static void mntzorro_write(unsigned base, unsigned reg, unsigned value)
{ assert(base == MNTZ_BASE_ADDR && reg == MNTZORRO_REG4 && !value && !running); clears++; }
static u16 ethernet_backlog_pending(void) { return frames_backlog + frames_backlog_reserved; }
static u16 ethernet_next_backlog_slot(u16 slot) { return (slot + 1u) % FRAME_MAX_BACKLOG; }
static void ethernet_send_pause_frame(void) { rx_pause_frames++; }
static u8 *ethernet_backlog_payload_ptr(u16 slot)
{ return (u8 *)(UINTPTR)(0x100000u + slot * FRAME_SIZE + RX_FRAME_PAD); }
/* FIRMWARE_HELPERS */

static LONG checked_create(XEmacPs_BdRing *r, UINTPTR phys, UINTPTR virt, u32 align, u32 count)
{
    assert(clears && !running); creates++;
    const char *stage = r == &EmacPsInstance.RxRing ? "rx-create" : "tx-create";
    /* Misalignment exercises the real Create rejection before touching storage. */
    return XEmacPs_BdRingCreate(r, phys, virt, failing(stage) ? 0 : align, count);
}
static LONG checked_clone(XEmacPs_BdRing *r, XEmacPs_Bd *pattern, u8 direction)
{
    assert(!running); clones++;
    if (failing(direction == XEMACPS_RECV ? "rx-clone" : "tx-clone")) r->RunState = XST_DMA_SG_IS_STARTED;
    LONG status = XEmacPs_BdRingClone(r, pattern, direction);
    if (status == XST_SUCCESS) {
        for (u32 i = 0; i < r->AllCnt; i++) {
            XEmacPs_Bd *bd = (XEmacPs_Bd *)(r->BaseBdAddr + i * r->Separation);
            if (direction == XEMACPS_RECV) {
                assert((*bd)[0] & XEMACPS_RXBUF_NEW_MASK);
                assert(!!((*bd)[0] & XEMACPS_RXBUF_WRAP_MASK) == (i + 1 == r->AllCnt));
            } else {
                assert((*bd)[1] & XEMACPS_TXBUF_USED_MASK);
                assert(!!((*bd)[1] & XEMACPS_TXBUF_WRAP_MASK) == (i + 1 == r->AllCnt));
            }
        }
    }
    return status;
}
static LONG checked_alloc(XEmacPs_BdRing *r, u32 count, XEmacPs_Bd **set)
{ allocs++; return XEmacPs_BdRingAlloc(r, failing("alloc") ? count + 1 : count, set); }
static int checked_prepare(XEmacPs_BdRing *r, XEmacPs_Bd *bd)
{
    if (failing("prepare") && prepared == prepare_failure) frames_backlog = ETH_BACKLOG_HIGH_WATERMARK;
    prepared++;
    return ethernet_prepare_rx_bd(r, bd);
}
static LONG checked_commit(XEmacPs_BdRing *r, u32 count, XEmacPs_Bd *set)
{
    assert(!running && !grants && !barriers && frames_backlog_reserved == RXBD_CNT);
    if (failing("commit")) r->PreHead = XEmacPs_BdRingNext(r, set);
    LONG status = XEmacPs_BdRingToHw(r, count, set);
    if (status == XST_SUCCESS) commits++;
    return status;
}
static void barrier(void) { assert(commits == 1 && !grants && !running); barriers++; }
static void XEmacPs_Start(XEmacPs *p)
{
    assert(p == &EmacPsInstance && !running);
    assert(commits == 1 && barriers == 1 && grants == RXBD_CNT);
    starts++; running = 1;
}
#define XEmacPs_BdRingCreate checked_create
#define XEmacPs_BdRingClone checked_clone
#define XEmacPs_BdRingAlloc checked_alloc
#define ethernet_prepare_rx_bd checked_prepare
#define XEmacPs_BdRingToHw checked_commit
#define dsb() barrier()
/* INIT_FUNCTION */

static void setup(void)
{
    (void)barrier; /* Keep the missing-barrier negative control warning-clean. */
    memset(&EmacPsInstance, 0, sizeof(EmacPsInstance));
    memset(rx, 0xa5, sizeof(rx)); memset(tx, 0x5a, sizeof(tx));
    stopped = running = clears = creates = clones = allocs = prepared = 0;
    commits = barriers = grants = starts = 0;
    failure = NULL; prepare_failure = 7;
    /* ethernet_init/restart callers initialize these before ring creation. */
    ethernet_clear_host_state();
    running = 1;
}
static void check_success(void)
{
    assert(init_ethernet_buffers() == XST_SUCCESS);
    assert(stopped == 1 && clears == 1 && creates == 2 && clones == 2);
    assert(allocs == 1 && prepared == RXBD_CNT && commits == 1 && barriers == 1);
    assert(grants == RXBD_CNT && starts == 1 && running);
    assert(!frames_backlog && frames_backlog_reserved == RXBD_CNT);
    assert(EmacPsInstance.RxRing.HwCnt == RXBD_CNT && !EmacPsInstance.RxRing.FreeCnt);
    assert(!EmacPsInstance.RxRing.PreCnt && !EmacPsInstance.RxRing.PostCnt);
    for (unsigned i = 0; i < RXBD_CNT; i++) assert(!(rx[i][0] & XEMACPS_RXBUF_NEW_MASK));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "success")) {
        setup(); check_success();
    } else {
        unsigned runs = !strcmp(argv[1], "prepare") ? 3 : 1;
        const unsigned positions[] = {0, 7, RXBD_CNT - 1};
        for (unsigned run = 0; run < runs; run++) {
            setup(); failure = argv[1]; prepare_failure = positions[run];
            assert(init_ethernet_buffers() == XST_FAILURE);
            assert(!running && !starts && !grants && !commits && !barriers);
            assert(!frames_backlog_reserved && !frames_backlog);
            if (failing("prepare") || failing("commit")) {
                assert(clears == 2 && EmacPsInstance.RxRing.FreeCnt == RXBD_CNT && !EmacPsInstance.RxRing.PreCnt);
                for (unsigned i = 0; i < RXBD_CNT; i++) {
                    assert(rx_bd_backlog_slot[i] == ETH_INVALID_BACKLOG_SLOT);
                    assert(rx[i][0] & XEMACPS_RXBUF_NEW_MASK);
                }
            }
        }
    }
    printf("PASS init %s\n", argv[1]);
    return 0;
}
