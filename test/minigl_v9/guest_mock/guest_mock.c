/*
 * Guest-client host harness: mock AmigaOS implementation.
 *
 * Provides the Amiga/P96 environment the real minigl.library backend and
 * the guest test clients expect, on top of an in-memory mock ZZ9000 card
 * supplied by the harness. Window and screen bitmaps live inside the mock
 * card memory so the firmware's MW_BLIT span writes land in the same
 * bytes p96ReadPixelArray observes, exactly like the real RTG card.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <graphics/layers.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <libraries/minigl_abi_types.h>

#include "guest_mock.h"

/* The real library lifecycle: the guest library's LibOpen runs MGLInit on
 * the 0 -> 1 open and LibClose runs AmiberryLibraryRelease on 1 -> 0
 * (minigl_library.c). The mock OpenLibrary/CloseLibrary reproduce that
 * policy for "minigl.library" only. */
GLboolean MGLInit(void);
void AmiberryResetPreferences(void);
void AmiberryLibraryRelease(void);

/* IntuitionBase/GfxBase/P96Base/LayersBase are defined by the library build
 * (client_backend.o); the mock and the guest clients share them exactly like
 * a resident library and its openers. DOSBase has no library-side definition
 * (AmigaOS startup provides it), so the mock owns it. */
struct DosLibrary *DOSBase;

/* ------------------------------------------------------------------ */
/* Mock card VRAM arena: bitmaps must live inside the board window the  */
/* firmware BLIT op writes to.                                          */
/* ------------------------------------------------------------------ */

static uint8_t *g_card_base;
static size_t   g_card_size;
static size_t   g_vram_cursor = 0x04000000; /* above FB base, below transport window */

void guest_mock_set_card(void *card_base, size_t card_size)
{
    g_card_base = (uint8_t *)card_base;
    g_card_size = card_size;
}

/* ARM-side address convention: board byte offset + address adjust, so the
 * transport's (span_addr - board_base) lands back on the card offset. */
#define MOCK_ADDR_ADJ 0x001F0000UL

static uint8_t *mock_vram_alloc(size_t bytes, ULONG *arm_base_out)
{
    size_t offset;
    if (!g_card_base) {
        if (arm_base_out) *arm_base_out = 0;
        return NULL;
    }
    offset = (g_vram_cursor + 15u) & ~(size_t)15u;
    if (offset + bytes > g_card_size) {
        if (arm_base_out) *arm_base_out = 0;
        return NULL;
    }
    g_vram_cursor = offset + bytes;
    if (arm_base_out) *arm_base_out = (ULONG)offset + MOCK_ADDR_ADJ;
    return g_card_base + offset;
}

/* ------------------------------------------------------------------ */
/* exec: libraries, memory, misc                                       */
/* ------------------------------------------------------------------ */

static struct Library g_lib_intuition;
static struct Library g_lib_graphics;
static struct Library g_lib_p96;
static struct Library g_lib_layers;
static struct Library g_lib_expansion;
static struct Library g_lib_dos;
static struct Library g_lib_minigl;

struct Library *OpenLibrary(STRPTR name, ULONG version)
{
    (void)version;
    if (!name)
        return NULL;
    if (strcmp(name, "minigl.library") == 0) {
        if (g_lib_minigl.lib_OpenCnt == 0) {
            if (!MGLInit())
                return NULL;
            AmiberryResetPreferences();
        }
        g_lib_minigl.lib_OpenCnt++;
        return &g_lib_minigl;
    }
    if (strcmp(name, "intuition.library") == 0) { g_lib_intuition.lib_OpenCnt++; return &g_lib_intuition; }
    if (strcmp(name, "graphics.library") == 0)  { g_lib_graphics.lib_OpenCnt++;  return &g_lib_graphics; }
    if (strcmp(name, "Picasso96API.library") == 0) { g_lib_p96.lib_OpenCnt++;    return &g_lib_p96; }
    if (strcmp(name, "layers.library") == 0)    { g_lib_layers.lib_OpenCnt++;    return &g_lib_layers; }
    if (strcmp(name, "expansion.library") == 0) { g_lib_expansion.lib_OpenCnt++; return &g_lib_expansion; }
    if (strcmp(name, "dos.library") == 0)       { g_lib_dos.lib_OpenCnt++;       return &g_lib_dos; }
    return NULL;
}

void CloseLibrary(struct Library *lib)
{
    if (!lib)
        return;
    if (lib == &g_lib_minigl) {
        if (g_lib_minigl.lib_OpenCnt == 0)
            return;
        g_lib_minigl.lib_OpenCnt--;
        if (g_lib_minigl.lib_OpenCnt == 0)
            AmiberryLibraryRelease();
        return;
    }
    if (lib->lib_OpenCnt)
        lib->lib_OpenCnt--;
}

void guest_mock_init_libraries(void)
{
    memset(&g_lib_intuition, 0, sizeof(g_lib_intuition));
    memset(&g_lib_graphics, 0, sizeof(g_lib_graphics));
    memset(&g_lib_p96, 0, sizeof(g_lib_p96));
    memset(&g_lib_layers, 0, sizeof(g_lib_layers));
    memset(&g_lib_expansion, 0, sizeof(g_lib_expansion));
    memset(&g_lib_dos, 0, sizeof(g_lib_dos));
    memset(&g_lib_minigl, 0, sizeof(g_lib_minigl));
    g_lib_minigl.lib_Version = 29;
    g_lib_minigl.lib_Revision = 1;
    g_vram_cursor = 0x04000000;
}

/* The Amiga is 32-bit: the library and guests cast pointers (windows,
 * screens, pixel buffers) into ULONG tag values and transport descriptors.
 * On a 64-bit host those casts truncate, so every pointer a guest may
 * truncate is allocated from one MAP_32BIT arena where truncation is
 * lossless, and variadic tag values are translated back through a
 * registry. Arena blocks are never freed: the harness runs once. */
static uint8_t *g_lowmem;
static size_t   g_lowmem_used;
static size_t   g_lowmem_size = 256UL * 1024UL * 1024UL;

static void *mock_arena_alloc(size_t bytes)
{
    size_t at;
    if (!g_lowmem) {
#ifdef MAP_32BIT
        g_lowmem = mmap(NULL, g_lowmem_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
        g_lowmem = mmap(NULL, g_lowmem_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (g_lowmem == MAP_FAILED) {
            g_lowmem = NULL;
            return NULL;
        }
    }
    at = (g_lowmem_used + 15u) & ~(size_t)15u;
    if (at + bytes > g_lowmem_size)
        return NULL;
    g_lowmem_used = at + bytes;
    return g_lowmem + at;
}

/* Pointers the guests may truncate to ULONG, registered at creation. */
#define MOCK_PTR_REGISTRY 256
static void *g_ptr_registry[MOCK_PTR_REGISTRY];

static void mock_register_ptr(void *ptr)
{
    int i;
    for (i = 0; i < MOCK_PTR_REGISTRY; ++i) {
        if (!g_ptr_registry[i]) {
            g_ptr_registry[i] = ptr;
            return;
        }
    }
}

static void mock_unregister_ptr(void *ptr)
{
    int i;
    for (i = 0; i < MOCK_PTR_REGISTRY; ++i)
        if (g_ptr_registry[i] == ptr)
            g_ptr_registry[i] = NULL;
}

static void *mock_ptr_from_u32(ULONG truncated)
{
    int i;
    for (i = 0; i < MOCK_PTR_REGISTRY; ++i)
        if (g_ptr_registry[i] && (ULONG)(uintptr_t)g_ptr_registry[i] == truncated)
            return g_ptr_registry[i];
    return NULL;
}

APTR AllocMem(ULONG size, ULONG flags)
{
    void *block = mock_arena_alloc(size ? (size_t)size : 1);
    if (block && (flags & MEMF_CLEAR))
        memset(block, 0, size);
    return block;
}

void FreeMem(APTR block, ULONG size)
{
    (void)block; (void)size; /* arena blocks live for the whole run */
}

APTR AllocVec(ULONG size, ULONG flags)
{
    return AllocMem(size, flags);
}

void FreeVec(APTR block)
{
    (void)block;
}

void CopyMem(const void *src, void *dst, ULONG size)
{
    if (src && dst && size)
        memcpy(dst, src, size);
}

void CopyMemQuick(const void *src, void *dst, ULONG size)
{
    CopyMem(src, dst, size);
}

void CacheClearE(APTR addr, ULONG length, ULONG flags)
{
    (void)addr; (void)length; (void)flags;
    __sync_synchronize();
}

void Forbid(void) {}
void Permit(void) {}
void Disable(void) {}
void Enable(void) {}

LONG AllocSignal(LONG sig)
{
    (void)sig;
    return 0;
}

void FreeSignal(LONG sig) { (void)sig; }

struct Message *GetMsg(struct MsgPort *port)
{
    (void)port;
    return NULL;
}

void WaitPort(struct MsgPort *port) { (void)port; }
void ReplyMsg(struct Message *msg) { (void)msg; }

/* --- timer.device --------------------------------------------------- */

static BYTE g_timer_req_pending;

LONG OpenDevice(STRPTR name, ULONG unit, struct IORequest *req, ULONG flags)
{
    (void)unit; (void)flags;
    if (!name || strcmp(name, "timer.device") != 0 || !req)
        return (BYTE)-1;
    req->io_Error = 0;
    req->io_Device = (struct Device *)&g_lib_dos; /* non-NULL marker */
    g_timer_req_pending = 0;
    return 0;
}

void CloseDevice(struct IORequest *req)
{
    if (req)
        req->io_Device = NULL;
}

BYTE DoIO(struct IORequest *req)
{
    struct timerequest *tr = (struct timerequest *)req;
    if (!req || !req->io_Device)
        return (BYTE)-1;
    if (req->io_Command == TR_GETSYSTIME) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        tr->tr_time.tv_secs = (ULONG)now.tv_sec;
        tr->tr_time.tv_micro = (ULONG)(now.tv_nsec / 1000);
    }
    req->io_Error = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* dos: in-memory files + result capture                               */
/* ------------------------------------------------------------------ */

struct mock_file {
    char  name[128];
    char *data;
    size_t size;
    size_t cap;
    size_t pos;
    int    open;
};

static struct mock_file g_files[32];
static char g_result_capture[65536];
static size_t g_result_capture_len;

const char *guest_mock_result_log(void)
{
    return g_result_capture;
}

void guest_mock_result_log_clear(void)
{
    g_result_capture_len = 0;
    g_result_capture[0] = '\0';
}

static struct mock_file *mock_file_find(const char *name)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_files) / sizeof(g_files[0])); ++i)
        if (g_files[i].open && strcmp(g_files[i].name, name) == 0)
            return &g_files[i];
    return NULL;
}

static struct mock_file *mock_file_create(const char *name)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_files) / sizeof(g_files[0])); ++i) {
        if (!g_files[i].open) {
            snprintf(g_files[i].name, sizeof(g_files[i].name), "%s", name);
            g_files[i].open = 1;
            return &g_files[i];
        }
    }
    return NULL;
}

BPTR Open(STRPTR name, LONG accessMode)
{
    struct mock_file *f;
    char path[120];

    if (!name)
        return 0;
    /* PROGDIR: maps to the in-memory guest result store. */
    if (strncmp(name, "PROGDIR:", 8) == 0)
        snprintf(path, sizeof(path), "%s", (const char *)name + 8);
    else
        snprintf(path, sizeof(path), "%s", (const char *)name);

    f = mock_file_find(path);
    if (!f) {
        f = mock_file_create(path);
        if (!f)
            return 0;
    }
    if (accessMode == MODE_NEWFILE) {
        f->size = 0;
        f->pos = 0;
    } else if (accessMode == MODE_READWRITE) {
        f->pos = f->size;
    } else {
        f->pos = 0;
    }
    return (BPTR)(intptr_t)(f - g_files) + 1;
}

static struct mock_file *mock_file_from_bptr(BPTR file)
{
    intptr_t index = (intptr_t)file - 1;
    if (file <= 0 || index >= (intptr_t)(sizeof(g_files) / sizeof(g_files[0])))
        return NULL;
    if (!g_files[index].open)
        return NULL;
    return &g_files[index];
}

LONG Close(BPTR file)
{
    (void)mock_file_from_bptr(file);
    return 1;
}

LONG Read(BPTR file, APTR buffer, LONG length)
{
    struct mock_file *f = mock_file_from_bptr(file);
    if (!f || !buffer || length < 0)
        return -1;
    if (f->pos + (size_t)length > f->size) {
        length = (LONG)(f->size - f->pos);
        if (length < 0)
            length = 0;
    }
    memcpy(buffer, f->data + f->pos, (size_t)length);
    f->pos += (size_t)length;
    return length;
}

LONG Write(BPTR file, APTR buffer, LONG length)
{
    struct mock_file *f = mock_file_from_bptr(file);
    if (!f || !buffer || length < 0)
        return -1;
    if (f->pos + (size_t)length > f->cap) {
        size_t cap = (f->pos + (size_t)length + 1023) & ~(size_t)1023;
        char *grown = (char *)realloc(f->data, cap);
        if (!grown)
            return -1;
        f->data = grown;
        f->cap = cap;
    }
    memcpy(f->data + f->pos, buffer, (size_t)length);
    f->pos += (size_t)length;
    if (f->pos > f->size)
        f->size = f->pos;
    /* Result clients write diagnostic shots too; capture only their text
     * result files so binary PPM bytes cannot truncate the C-string log. */
    if (strstr(f->name, "-result.txt") != NULL) {
        size_t room = sizeof(g_result_capture) - g_result_capture_len - 1;
        size_t take = (size_t)length < room ? (size_t)length : room;
        memcpy(g_result_capture + g_result_capture_len, buffer, take);
        g_result_capture_len += take;
        g_result_capture[g_result_capture_len] = '\0';
    }
    return length;
}

LONG Seek(BPTR file, LONG position, LONG offset)
{
    struct mock_file *f = mock_file_from_bptr(file);
    LONG old;
    if (!f)
        return -1;
    old = (LONG)f->pos;
    if (offset == OFFSET_BEGINNING)
        f->pos = (size_t)position;
    else if (offset == OFFSET_CURRENT)
        f->pos = (size_t)((LONG)f->pos + position);
    else if (offset == OFFSET_END)
        f->pos = f->size + (size_t)position;
    return old;
}

BPTR Output(void) { return (BPTR)1; }
BPTR Input(void) { return (BPTR)2; }

LONG GetVar(CONST_STRPTR name, STRPTR buffer, LONG size, ULONG flags)
{
    (void)name; (void)buffer; (void)size; (void)flags;
    return -1; /* not found: guests fall back to PROGDIR: */
}

void DateStamp(struct DateStamp *stamp)
{
    if (stamp) {
        stamp->ds_Days = 0;
        stamp->ds_Minute = 0;
        stamp->ds_Tick = 0;
    }
}
LONG DeleteFile(STRPTR name)
{
    (void)name;
    return 1;
}

static struct Process g_mock_process;

struct Task *FindTask(STRPTR name)
{
    if (name)
        return NULL;
    return (struct Task *)&g_mock_process;
}

/* --- displayinfo database: one fake 800x600 RTG mode ---------------- */

ULONG NextDisplayInfo(ULONG last_id)
{
    return last_id == 0 ? 0x50001000 : (ULONG)INVALID_ID;
}

LONG GetDisplayInfoData(void *handle, UBYTE *buffer, LONG size, ULONG tag, ULONG id)
{
    struct DimensionInfo *dims;
    (void)handle; (void)tag;
    if (id != 0x50001000 || !buffer || size < (LONG)sizeof(*dims))
        return 0;
    dims = (struct DimensionInfo *)buffer;
    memset(dims, 0, sizeof(*dims));
    dims->Nominal.MaxX = 799;
    dims->Nominal.MaxY = 599;
    return (LONG)sizeof(*dims);
}

/* ------------------------------------------------------------------ */
/* graphics / intuition: screens, windows, bitmaps                     */
/* ------------------------------------------------------------------ */

static struct Screen g_workbench;
static int g_workbench_ready;

static void mock_bitmap_init(struct BitMap *bm, ULONG width, ULONG height,
                             ULONG format, ULONG bytes_per_pixel)
{
    size_t bytes;
    memset(bm, 0, sizeof(*bm));
    bm->mock_pitch = width * bytes_per_pixel;
    bm->mock_width = width;
    bm->mock_height = height;
    bm->mock_format = format;
    bm->BytesPerRow = (UWORD)bm->mock_pitch;
    bm->Rows = (UWORD)height;
    bm->Depth = 32;
    bytes = (size_t)bm->mock_pitch * height;
    bm->mock_memory = mock_vram_alloc(bytes, &bm->mock_arm_base);
    if (bm->mock_memory)
        memset(bm->mock_memory, 0, bytes);
}

static struct BitMap *mock_bitmap_alloc(ULONG width, ULONG height, ULONG format,
                                        ULONG bytes_per_pixel)
{
    struct BitMap *bm = (struct BitMap *)mock_arena_alloc(sizeof(*bm));
    if (!bm)
        return NULL;
    memset(bm, 0, sizeof(*bm));
    mock_bitmap_init(bm, width, height, format, bytes_per_pixel);
    if (!bm->mock_memory)
        return NULL;
    mock_register_ptr(bm);
    return bm;
}

static void guest_mock_workbench(void)
{
    if (g_workbench_ready)
        return;
    memset(&g_workbench, 0, sizeof(g_workbench));
    g_workbench.Width = 800;
    g_workbench.Height = 600;
    g_workbench.RastPort.BitMap = mock_bitmap_alloc(800, 600, RGBFB_R8G8B8A8, 4);
    mock_register_ptr(&g_workbench);
    g_workbench_ready = 1;
}

void InitRastPort(struct RastPort *rp)
{
    if (rp)
        memset(rp, 0, sizeof(*rp));
}

LONG GetBitMapAttr(struct BitMap *bitmap, LONG attr)
{
    if (!bitmap)
        return 0;
    if (attr == BMA_DEPTH)
        return (LONG)bitmap->Depth;
    return 0;
}

void WaitTOF(void)
{
    /* A real vertical blank is ~20 ms; the transport's frame-counted
     * timeouts assume that scale. 2 ms keeps the harness quick while
     * giving the worker thread the order of a second per 100-frame wait,
     * so ring backpressure never drops packets the card would have
     * drained in time. */
    usleep(2000);
}

void SetAPen(struct RastPort *rp, ULONG pen)
{
    if (rp)
        rp->pen = (UBYTE)pen;
}

void RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1)
{
    /* The library only clears the window before a letterboxed present. */
    struct BitMap *bm;
    LONG x, y;
    if (!rp || !rp->BitMap || !rp->BitMap->mock_memory)
        return;
    bm = rp->BitMap;
    for (y = y0; y <= y1; ++y) {
        if (y < 0 || (ULONG)y >= bm->mock_height)
            continue;
        for (x = x0; x <= x1; ++x) {
            if (x < 0 || (ULONG)x >= bm->mock_width)
                continue;
            memset(bm->mock_memory + (size_t)y * bm->mock_pitch +
                   (size_t)x * 4, 0, 4);
        }
    }
}

void LockLayer(LONG dummy, struct Layer *layer)
{
    (void)dummy; (void)layer;
}

void UnlockLayer(struct Layer *layer)
{
    (void)layer;
}

ULONG BestModeID(Tag tag1, ...)
{
    (void)tag1;
    return 0x50001000; /* a fake P96 mode id */
}

ULONG GetVPModeID(struct ViewPort *vp)
{
    (void)vp;
    return 0x50001000;
}

/* --- screens -------------------------------------------------------- */

struct Screen *OpenScreenTags(void *newscreen, Tag tag1, ...)
{
    struct Screen *screen;
    va_list ap;
    Tag tag = tag1;
    ULONG display_id = 0x50001000;
    LONG width = STDSCREENWIDTH;
    LONG height = STDSCREENHEIGHT;
    ULONG depth = 24;

    (void)newscreen;
    va_start(ap, tag1);
    while (tag != TAG_DONE) {
        ULONG value = va_arg(ap, ULONG);
        if (tag == SA_DisplayID)
            display_id = value;
        else if (tag == SA_Width)
            width = (LONG)value;
        else if (tag == SA_Height)
            height = (LONG)value;
        else if (tag == SA_Depth)
            depth = value;
        tag = va_arg(ap, Tag);
    }
    va_end(ap);

    (void)display_id; (void)depth;
    if (width == (LONG)STDSCREENWIDTH)
        width = 800;
    if (height == (LONG)STDSCREENHEIGHT)
        height = 600;

    screen = (struct Screen *)mock_arena_alloc(sizeof(*screen));
    if (!screen)
        return NULL;
    memset(screen, 0, sizeof(*screen));
    screen->Width = (WORD)width;
    screen->Height = (WORD)height;
    InitRastPort(&screen->RastPort);
    screen->RastPort.BitMap = mock_bitmap_alloc((ULONG)width, (ULONG)height,
                                                RGBFB_R8G8B8A8, 4);
    if (!screen->RastPort.BitMap)
        return NULL;
    mock_register_ptr(screen);
    return screen;
}

void CloseScreen(struct Screen *screen)
{
    if (!screen)
        return;
    mock_unregister_ptr(screen);
}

struct Screen *LockPubScreen(STRPTR name)
{
    (void)name;
    guest_mock_workbench();
    if (!g_workbench.RastPort.BitMap)
        return NULL;
    return &g_workbench;
}

void UnlockPubScreen(void *unused, struct Screen *screen)
{
    (void)unused; (void)screen;
}

void ScreenToFront(struct Screen *screen) { (void)screen; }

BOOL OpenWorkBench(void) { return TRUE; }
BOOL CloseWorkBench(void) { return TRUE; }

/* --- windows -------------------------------------------------------- */

struct Window *OpenWindowTags(void *newwin, Tag tag1, ...)
{
    struct Window *window;
    struct Layer *layer;
    struct ClipRect *clip;
    va_list ap;
    Tag tag = tag1;
    LONG left = 0, top = 0, width = 0, height = 0;
    BOOL have_inner = 0;
    ULONG idcmp = 0;
    struct Screen *custom = NULL;

    (void)newwin;
    va_start(ap, tag1);
    while (tag != TAG_DONE) {
        ULONG value = va_arg(ap, ULONG);
        if (tag == WA_Left)
            left = (LONG)value;
        else if (tag == WA_Top)
            top = (LONG)value;
        else if (tag == WA_Width)
            width = (LONG)value;
        else if (tag == WA_Height)
            height = (LONG)value;
        else if (tag == WA_InnerWidth) {
            width = (LONG)value;
            have_inner = 1;
        } else if (tag == WA_InnerHeight) {
            height = (LONG)value;
            have_inner = 1;
        } else if (tag == WA_IDCMP)
            idcmp = value;
        else if (tag == WA_CustomScreen)
            custom = (struct Screen *)mock_ptr_from_u32(value);
        tag = va_arg(ap, Tag);
    }
    va_end(ap);

    if (have_inner) {
        /* Borders are zero in this mock, so inner == outer. */
    }
    if (width <= 0 || height <= 0)
        return NULL;

    window = (struct Window *)mock_arena_alloc(sizeof(*window));
    layer = (struct Layer *)mock_arena_alloc(sizeof(*layer));
    clip = (struct ClipRect *)mock_arena_alloc(sizeof(*clip));
    if (!window || !layer || !clip)
        return NULL;
    memset(window, 0, sizeof(*window));
    memset(layer, 0, sizeof(*layer));
    memset(clip, 0, sizeof(*clip));
    window->LeftEdge = (WORD)left;
    window->TopEdge = (WORD)top;
    window->Width = (WORD)width;
    window->Height = (WORD)height;
    window->IDCMPFlags = idcmp;
    window->WScreen = custom ? custom : LockPubScreen(NULL);
    window->UserPort = (void *)(uintptr_t)1; /* port exists for MGLMainLoop */
    window->RPort = (struct RastPort *)mock_arena_alloc(sizeof(*window->RPort));
    if (!window->RPort)
        return NULL;
    memset(window->RPort, 0, sizeof(*window->RPort));
    window->RPort->BitMap = mock_bitmap_alloc((ULONG)width, (ULONG)height,
                                              RGBFB_R8G8B8A8, 4);
    if (!window->RPort->BitMap)
        return NULL;
    /* One clip rect covering the whole window, never obscured. */
    clip->bounds.MinX = (WORD)left;
    clip->bounds.MinY = (WORD)top;
    clip->bounds.MaxX = (WORD)(left + width - 1);
    clip->bounds.MaxY = (WORD)(top + height - 1);
    layer->ClipRect = clip;
    window->RPort->Layer = layer;
    return window;
}

void CloseWindow(struct Window *window)
{
    if (!window)
        return;
    if (window->RPort && window->RPort->BitMap)
        mock_unregister_ptr(window->RPort->BitMap);
    mock_unregister_ptr(window);
}

void ClearPointer(struct Window *window)
{
    (void)window;
}

void SetPointer(struct Window *window, UWORD *sprite, LONG width, LONG height,
                LONG xoffset, LONG yoffset)
{
    (void)window; (void)sprite; (void)width; (void)height;
    (void)xoffset; (void)yoffset;
}

BOOL ModifyIDCMP(struct Window *window, ULONG flags)
{
    if (!window)
        return FALSE;
    window->IDCMPFlags = flags;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Picasso96 API                                                       */
/* ------------------------------------------------------------------ */

ULONG p96GetModeIDAttr(ULONG modeid, ULONG attr)
{
    if (attr == P96IDA_ISP96)
        return 1;
    if (attr == P96IDA_DEPTH)
        return 32;
    (void)modeid;
    return 0;
}

ULONG p96BestModeIDTags(Tag tag1, ...)
{
    (void)tag1;
    return 0x50001000;
}

LONG p96GetBitMapAttr(struct BitMap *bitmap, ULONG attr)
{
    if (!bitmap)
        return 0;
    switch (attr) {
    case P96BMA_MEMORY:        return (LONG)(uintptr_t)bitmap->mock_memory;
    case P96BMA_BYTESPERROW:   return (LONG)bitmap->mock_pitch;
    case P96BMA_BYTESPERPIXEL: return 4;
    case P96BMA_RGBFORMAT:     return (LONG)bitmap->mock_format;
    case P96BMA_WIDTH:         return (LONG)bitmap->mock_width;
    case P96BMA_HEIGHT:        return (LONG)bitmap->mock_height;
    default:                   return 0;
    }
}

LONG p96LockBitMap(struct BitMap *bitmap, void *lockinfo, ULONG flags)
{
    (void)lockinfo; (void)flags;
    return bitmap ? 1 : 0;
}

void p96UnlockBitMap(struct BitMap *bitmap, LONG lock)
{
    (void)bitmap; (void)lock;
}

/* RenderInfo memory is RGBA8 rows of BytesPerRow; the source rectangle is
 * (destx, desty) in the RastPort, landing at (srcx, srcy) in the array. */
void p96ReadPixelArray(struct RenderInfo *ri, ULONG srcx, ULONG srcy,
                       struct RastPort *rp, ULONG destx, ULONG desty,
                       ULONG width, ULONG height)
{
    struct BitMap *bm;
    ULONG row;
    if (!ri || !rp || !rp->BitMap || !ri->Memory)
        return;
    bm = rp->BitMap;
    for (row = 0; row < height; ++row) {
        LONG sy = (LONG)(desty + row);
        UBYTE *out = ri->Memory + (size_t)(srcy + row) * ri->BytesPerRow + srcx * 4;
        LONG sx;
        if (sy < 0 || (ULONG)sy >= bm->mock_height) {
            memset(out, 0, width * 4);
            continue;
        }
        for (sx = 0; (ULONG)sx < width; ++sx) {
            LONG x = (LONG)(destx + (ULONG)sx);
            if (x < 0 || (ULONG)x >= bm->mock_width) {
                memset(out + (size_t)sx * 4, 0, 4);
            } else {
                memcpy(out + (size_t)sx * 4,
                       bm->mock_memory + (size_t)sy * bm->mock_pitch + (size_t)x * 4,
                       4);
            }
        }
    }
}

void p96WritePixelArray(struct RenderInfo *ri, ULONG srcx, ULONG srcy,
                        struct RastPort *rp, ULONG destx, ULONG desty,
                        ULONG width, ULONG height)
{
    struct BitMap *bm;
    ULONG row;
    if (!ri || !rp || !rp->BitMap || !ri->Memory)
        return;
    bm = rp->BitMap;
    for (row = 0; row < height; ++row) {
        LONG dy = (LONG)(desty + row);
        const UBYTE *src = ri->Memory + (size_t)(srcy + row) * ri->BytesPerRow + srcx * 4;
        if (dy < 0 || (ULONG)dy >= bm->mock_height)
            continue;
        {
            ULONG x;
            for (x = 0; x < width; ++x) {
                LONG dx = (LONG)(destx + x);
                if (dx < 0 || (ULONG)dx >= bm->mock_width)
                    continue;
                memcpy(bm->mock_memory + (size_t)dy * bm->mock_pitch + (size_t)dx * 4,
                       src + (size_t)x * 4, 4);
            }
        }
    }
}

struct BitMap *p96AllocBitMap(ULONG width, ULONG height, ULONG depth,
                              ULONG flags, struct BitMap *friend,
                              ULONG rgbformat)
{
    (void)depth; (void)flags; (void)friend;
    if (rgbformat < RGBFB_R8G8B8 || rgbformat > RGBFB_B5G5R5PC)
        rgbformat = RGBFB_R8G8B8A8;
    return mock_bitmap_alloc(width, height, rgbformat, 4);
}

void p96FreeBitMap(struct BitMap *bitmap)
{
    mock_unregister_ptr(bitmap);
}
