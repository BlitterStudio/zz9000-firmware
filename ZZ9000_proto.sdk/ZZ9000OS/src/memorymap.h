
// FIXME allocate this memory properly

#define AUDIO_NUM_PERIODS           8
#define AUDIO_BYTES_PER_PERIOD      3840

#define FRAMEBUFFER_ADDRESS         0x00200000
#define AUDIO_TX_BUFFER_SIZE        (AUDIO_BYTES_PER_PERIOD * AUDIO_NUM_PERIODS)

#define Z3_SCRATCH_ADDR             0x033F0000 // FIXME @ _Bnu
#define ADDR_ADJ                    0x001F0000 // FIXME @ _Bnu

#define AUDIO_TX_BUFFER_ADDRESS     0x3FC00000 // default, changed by driver
#define AUDIO_RX_BUFFER_ADDRESS     0x3FC20000 // default, changed by driver
#define TX_BD_LIST_START_ADDRESS    0x3FD00000
#define RX_BD_LIST_START_ADDRESS    0x3FD08000
#define TX_FRAME_ADDRESS            0x3FD10000
#define RX_FRAME_ADDRESS            0x3FD20000
#define RX_BACKLOG_ADDRESS          0x3FE00000 // 128 * 2048 space (256 kB) — must match FRAME_MAX_BACKLOG
#define USB_BLOCK_STORAGE_ADDRESS   0x3FE40000 // must sit past RX_BACKLOG; same 1 MB MMU page as 0x3FE00000
#define BOOT_ROM_ADDRESS            0x3FCF0000
#define RX_FRAME_PAD 4
#define FRAME_SIZE 2048

// Our address space is relative to the autoconfig base address (for example, it could be 0x600000)
#define MNT_REG_BASE    			0x00000000

// 0x2000 - 0x7fff   ETH RX
// 0x8000 - 0x9fff   ETH TX
// 0xa000 - 0xffff   USB BLOCK

// Frame buffer/graphics memory starts at 64KB (relative to card address), leaving ample space for general purpose registers.
#define MNT_FB_BASE     			0x00010000
