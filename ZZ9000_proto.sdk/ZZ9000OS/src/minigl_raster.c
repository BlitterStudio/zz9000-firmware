/* ZZ9000 ARM Software 3D Rasterizer & Pipeline. */
#include "minigl_raster.h"
#include "minigl_present.h"
#include "minigl_lighting.h"
#include "mw_types.h"
#include "mw_state.h"
#include "mw_draw.h"

#include <limits.h>
#include <math.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define MAX_TMU 2U
#define MAX_TEXTURES ZZ9K_MINIGL_MAX_TEXTURES
#define VERTEX_CACHE_SIZE 128U
#define MATRIX_STACK_DEPTH 32U
#define RASTER_MAX_WIDTH 1024U
#define RASTER_MAX_HEIGHT 768U
#define CLIP_MAX_VERTICES 32U

#define GL_POINTS 0x0000U
#define GL_LINES 0x0001U
#define GL_LINE_LOOP 0x0002U
#define GL_LINE_STRIP 0x0003U
#define GL_TRIANGLES 0x0004U
#define GL_TRIANGLE_STRIP 0x0005U
#define GL_TRIANGLE_FAN 0x0006U
#define GL_QUADS 0x0007U
#define GL_QUAD_STRIP 0x0008U
#define GL_POLYGON 0x0009U
#define GL_TEXTURE_2D 0x0de1U
#define GL_MODULATE 0x2100U
#define GL_DECAL 0x2101U
#define GL_REPLACE 0x1e01U
#define GL_ADD 0x0104U
#define GL_TEXTURE_ENV_BLEND 0x0be2U
#define GL_NEAREST 0x2600U
#define GL_LINEAR 0x2601U
#define GL_NEAREST_MIPMAP_NEAREST 0x2700U
#define GL_LINEAR_MIPMAP_NEAREST 0x2701U
#define GL_NEAREST_MIPMAP_LINEAR 0x2702U
#define GL_LINEAR_MIPMAP_LINEAR 0x2703U
#define GL_EXP 0x0800U
#define GL_EXP2 0x0801U
#define GL_REPEAT 0x2901U
#define GL_CLAMP 0x2900U
#define GL_CLAMP_TO_EDGE 0x812fU
#define GL_FRONT 0x0404U
#define GL_BACK 0x0405U
#define GL_FRONT_AND_BACK 0x0408U
#define GL_CCW 0x0901U
#define GL_FLAT 0x1d00U
#define GL_POINT 0x1b00U
#define GL_LINE 0x1b01U
#define GL_FILL 0x1b02U
#define GL_NEVER 0x0200U
#define GL_LESS 0x0201U
#define GL_EQUAL 0x0202U
#define GL_LEQUAL 0x0203U
#define GL_GREATER 0x0204U
#define GL_NOTEQUAL 0x0205U
#define GL_GEQUAL 0x0206U
#define GL_ALWAYS 0x0207U
#define GL_ZERO 0U
#define GL_ONE 1U
#define GL_SRC_COLOR 0x0300U
#define GL_ONE_MINUS_SRC_COLOR 0x0301U
#define GL_SRC_ALPHA 0x0302U
#define GL_ONE_MINUS_SRC_ALPHA 0x0303U
#define GL_DST_ALPHA 0x0304U
#define GL_ONE_MINUS_DST_ALPHA 0x0305U
#define GL_DST_COLOR 0x0306U
#define GL_ONE_MINUS_DST_COLOR 0x0307U
#define GL_SRC_ALPHA_SATURATE 0x0308U
#define GL_FUNC_ADD 0x8006U
#define GL_MIN 0x8007U
#define GL_MAX 0x8008U
#define GL_FUNC_SUBTRACT 0x800aU
#define GL_FUNC_REVERSE_SUBTRACT 0x800bU
#define GL_COLOR_BUFFER_BIT 0x00004000U
#ifndef ZZ9K_MGL_TEX_INTERNAL_RGB
#define ZZ9K_MGL_TEX_INTERNAL_RGB 0x8000U
#define ZZ9K_MGL_TEX_STORAGE_MASK 0x7fffU
#endif
#define GL_DEPTH_BUFFER_BIT 0x00000100U

#define RASTER_ERR_MEMORY 1U
#define RASTER_ERR_STATE 2U
#define RASTER_ERR_TEXTURE 4U
#define RASTER_ERR_VERTEX 8U
#define RASTER_ERR_MATRIX 16U

typedef struct TextureLevel {
    uint32_t offset;
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint8_t internal_rgb;
    struct TextureLevel *next;
} TextureLevel;

typedef struct Texture {
    uint32_t id;
    TextureLevel levels[ZZ9K_MINIGL_MAX_MIP_LEVELS];
    uint16_t min_filter;
    uint16_t mag_filter;
    uint16_t wrap_s;
    uint16_t wrap_t;
    uint8_t valid;
    uint8_t complete;
    uint8_t last_level;
} Texture;

typedef struct Vertex {
    float x;
    float y;
    float z;
    float w;
    float inv_w;
    float sx;
    float sy;
    float sz;
    float u0;
    float v0;
    float q0;
    float u1;
    float v1;
    float q1;
    float fog_coord;
    uint32_t color;
    uint32_t front_color;
    uint32_t back_color;
    uint32_t flat_color;
} Vertex;

static MiniGLRasterBuffers g_buffers;
static ZZ9KMiniGLPacketSetState g_state;
static Texture g_textures[MAX_TEXTURES];
static Texture g_default_texture;
static TextureLevel *g_texture_allocations;
static uint32_t g_texture_used;
static uint16_t g_texture_lookup[MAX_TEXTURES * 2U];
static Vertex g_vertex_cache[VERTEX_CACHE_SIZE];
static uint32_t g_vertex_cache_index[VERTEX_CACHE_SIZE];
static uint32_t g_vertex_cache_epoch[VERTEX_CACHE_SIZE];
static uint32_t g_draw_epoch;
static uint16_t g_draw_attr_code;
static uint32_t g_bound_texture[MAX_TMU];
static Texture *g_bound_texture_info[MAX_TMU];
static float g_matrix[4][16];
static float g_mvp[16];
static float g_stack[4][MATRIX_STACK_DEPTH][16];
static uint8_t g_stack_depth[4];
static uint8_t g_depth_bits;
static float g_polygon_offset_units;
static float g_polygon_depth_scale;
#if defined(__ARM_NEON)
static int g_mvp_neon_ok;
#endif
static const mw_state *g_mw_state = NULL;

void minigl_raster_set_wire_v9_state(const struct mw_state *state)
{
    g_mw_state = state;
}

static uint32_t g_error;
static MiniGLRasterDirtyRegion g_dirty;
static int g_dirty_valid;
static MiniGLRasterTraceHook g_trace_hook;
static MiniGLRixBackend g_rix;
static MiniGLRixPlatform g_rix_platform;
static int g_rix_platform_set;
static uint8_t *g_private_texture_pool;
static uint32_t g_private_texture_pool_bytes;
static MiniGLRasterTexturePoolGrowFn g_texture_pool_grow;
static void *g_texture_pool_grow_context;
/* Positive while assemble emits setup triangles, negative on capacity failure. */
static int g_rix_collecting;
static uint32_t g_frame_draws_hardware = 0U;
static uint32_t g_frame_draws_software = 0U;
static uint32_t g_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
static uint32_t g_last_frame_draws_hardware = 0U;
static uint32_t g_last_frame_draws_software = 0U;
static uint32_t g_last_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
static uint32_t g_logged_fallback_mask = 0U;
static int g_context_backend_logged = 0;

/* A CPU fallback drains and converts the whole shared target.  Finish that
 * hand-off once, then keep the frame on CPU; bouncing after every primitive
 * would cost a full target READ/format conversion per draw. */
#define MINIGL_RIX_CPU_FALLBACKS_PER_FRAME 1U
static uint32_t g_rix_cpu_fallbacks;
static int g_rix_cpu_frame;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static float bef(const uint8_t *p)
{
    union { uint32_t u; float f; } value;
    value.u = be32(p);
    return value.f;
}

#if defined(__ARM_NEON)
static void be_float4(const uint8_t *source, float *out)
{
    uint8x16_t bytes = vrev32q_u8(vld1q_u8(source));
    vst1q_u8((uint8_t *)out, bytes);
}
#endif

static uint8_t channel(uint32_t color, unsigned shift)
{
    return (uint8_t)(color >> shift);
}

static uint32_t rgba(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    return ((uint32_t)red << 24) | ((uint32_t)green << 16) |
           ((uint32_t)blue << 8) | alpha;
}

static inline float fast_floorf(float x)
{
    int i = (int)x;
    return (float)(i - (x < (float)i));
}

static uint8_t expand5(uint8_t value) { return (uint8_t)((value << 3) | (value >> 2)); }
static uint8_t expand6(uint8_t value) { return (uint8_t)((value << 2) | (value >> 4)); }
static inline uint8_t mul8(uint8_t a, uint8_t b)
{
    uint32_t p = (uint32_t)a * b + 128U;
    return (uint8_t)((p + (p >> 8)) >> 8);
}
static uint8_t satadd(uint8_t a, uint8_t b)
{
    uint16_t sum = (uint16_t)a + b;
    return (uint8_t)(sum > 255U ? 255U : sum);
}

static float clamp01(float value)
{
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static void identity(float *matrix)
{
    memset(matrix, 0, 16U * sizeof(*matrix));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void matrix_mul(float *out, const float *left, const float *right)
{
    float value[16];
    unsigned row;
    unsigned column;
    unsigned term;

    for (column = 0; column < 4U; ++column) {
        for (row = 0; row < 4U; ++row) {
            value[column * 4U + row] = 0.0f;
            for (term = 0; term < 4U; ++term)
                value[column * 4U + row] += left[term * 4U + row] * right[column * 4U + term];
        }
    }
    memcpy(out, value, sizeof(value));
}

#if defined(__ARM_NEON)
static int matrix_neon_ok(const float *matrix)
{
    unsigned index;
    for (index = 0U; index < 16U; ++index)
        if (!isfinite(matrix[index]) || fpclassify(matrix[index]) == FP_SUBNORMAL)
            return 0;
    return 1;
}
#endif

static void update_mvp(void)
{
    matrix_mul(g_mvp, g_matrix[1], g_matrix[0]);
#if defined(__ARM_NEON)
    g_mvp_neon_ok = matrix_neon_ok(g_mvp);
#endif
}
static int mode_ok(uint16_t mode) { return mode < 4U; }
static uint32_t pixels(void) { return (uint32_t)g_buffers.width * g_buffers.height; }
static uint32_t depth_bytes_per_pixel(void) { return g_depth_bits == 16U ? 2U : 3U; }
static uint32_t depth_max(void) { return g_depth_bits == 16U ? 0xffffU : 0xffffffU; }
static int ready(void);
void minigl_raster_set_texture_pool(uint8_t *pool, uint32_t bytes,
                                    MiniGLRasterTexturePoolGrowFn grow,
                                    void *grow_context)
{
    g_private_texture_pool = pool;
    g_private_texture_pool_bytes = bytes;
    g_texture_pool_grow = grow;
    g_texture_pool_grow_context = grow_context;
}

void minigl_raster_set_rix_platform(const MiniGLRixPlatform *platform)
{
    if (platform == NULL) {
        memset(&g_rix_platform, 0, sizeof(g_rix_platform));
        g_rix_platform_set = 0;
        return;
    }
    g_rix_platform = *platform;
    g_rix_platform_set = 1;
}

int minigl_raster_finish(void)
{
    if (!ready())
        return 0;
    if (!minigl_rix_backend_make_cpu_visible(&g_rix, g_buffers.color, pixels())) {
        g_error |= RASTER_ERR_MEMORY;
        return 0;
    }
    return 1;
}

static int rix_cpu_visible(void)
{
    return minigl_raster_finish();
}


void minigl_raster_set_trace_hook(MiniGLRasterTraceHook hook)
{
    g_trace_hook = hook;
}

static void mark_dirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g_buffers.width) x1 = (int)g_buffers.width - 1;
    if (y1 >= g_buffers.height) y1 = (int)g_buffers.height - 1;
    if (x0 > x1 || y0 > y1)
        return;
    if (!g_dirty_valid) {
        g_dirty.x = (uint16_t)x0; g_dirty.y = (uint16_t)y0;
        g_dirty.width = (uint16_t)(x1 - x0 + 1); g_dirty.height = (uint16_t)(y1 - y0 + 1);
        g_dirty_valid = 1;
        return;
    }
    { int right = g_dirty.x + g_dirty.width - 1, bottom = g_dirty.y + g_dirty.height - 1;
      if (x0 < g_dirty.x) g_dirty.x = (uint16_t)x0;
      if (y0 < g_dirty.y) g_dirty.y = (uint16_t)y0;
      if (x1 > right) right = x1;
      if (y1 > bottom) bottom = y1;
      g_dirty.width = (uint16_t)(right - g_dirty.x + 1); g_dirty.height = (uint16_t)(bottom - g_dirty.y + 1); }
}

int minigl_raster_take_dirty_region(MiniGLRasterDirtyRegion *region)
{
    if (!region || !g_dirty_valid)
        return 0;
    *region = g_dirty;
    g_dirty_valid = 0;
    return 1;
}

static int ready(void)
{
    return g_buffers.color && g_buffers.depth && g_buffers.vertices &&
           g_buffers.texture_staging && g_buffers.texture_pool &&
           g_buffers.width && g_buffers.height;
}

int minigl_raster_configure_buffers(const MiniGLRasterBuffers *buffers)
{
    uint32_t count;

    if (!buffers || !buffers->color || !buffers->depth || !buffers->vertices ||
        !buffers->texture_staging || !buffers->texture_pool || !buffers->width ||
        !buffers->height || buffers->width > RASTER_MAX_WIDTH ||
        buffers->height > RASTER_MAX_HEIGHT) {
        g_error |= RASTER_ERR_MEMORY;
        return 0;
    }
    count = (uint32_t)buffers->width * buffers->height;
    if (count > buffers->color_bytes / sizeof(*buffers->color) ||
        count > buffers->depth_bytes / 3U || !buffers->vertices_bytes ||
        !buffers->texture_staging_bytes || !buffers->texture_pool_bytes) {
        g_error |= RASTER_ERR_MEMORY;
        return 0;
    }
    g_buffers = *buffers;
    return 1;
}

void minigl_raster_release_buffers(void)
{
    memset(&g_buffers, 0, sizeof(g_buffers));
    memset(g_textures, 0, sizeof(g_textures));
    memset(&g_default_texture, 0, sizeof(g_default_texture));
    g_texture_allocations = NULL;
    g_texture_used = 0U;
    memset(g_texture_lookup, 0, sizeof(g_texture_lookup));
    memset(g_bound_texture, 0, sizeof(g_bound_texture));
    memset(g_bound_texture_info, 0, sizeof(g_bound_texture_info));
    g_dirty_valid = 0;
}

const uint32_t *minigl_raster_color_buffer(void) { return g_buffers.color; }
uint16_t minigl_raster_width(void) { return g_buffers.width; }
uint16_t minigl_raster_height(void) { return g_buffers.height; }
uint32_t minigl_raster_get_error(void)
{
    uint32_t error = g_error;
    g_error = 0U;
    return error;
}

int minigl_raster_configure(uint32_t width, uint32_t height)
{
    MiniGLRasterBuffers buffers;

    if (!width || !height || width > ZZ9K_MINIGL_MAX_WIDTH ||
        height > ZZ9K_MINIGL_MAX_HEIGHT || width > UINT16_MAX || height > UINT16_MAX ||
        !ready()) {
        g_error |= RASTER_ERR_MEMORY;
        return 0;
    }
    g_frame_draws_hardware = 0U;
    g_frame_draws_software = 0U;
    g_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    g_last_frame_draws_hardware = 0U;
    g_last_frame_draws_software = 0U;
    g_last_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    g_logged_fallback_mask = 0U;
    g_context_backend_logged = 0;
    g_rix_cpu_fallbacks = 0U;
    g_rix_cpu_frame = 0;
    buffers = g_buffers;
    buffers.width = (uint16_t)width;
    buffers.height = (uint16_t)height;
    return minigl_raster_configure_buffers(&buffers);
}

void minigl_raster_reset(void)
{
    (void)minigl_raster_finish();
    minigl_rix_backend_reset(&g_rix);
    g_error = 0U;
    g_frame_draws_hardware = 0U;
    g_frame_draws_software = 0U;
    g_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    g_last_frame_draws_hardware = 0U;
    g_last_frame_draws_software = 0U;
    g_last_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    g_logged_fallback_mask = 0U;
    g_context_backend_logged = 0;
    g_rix_cpu_fallbacks = 0U;
    g_rix_cpu_frame = 0;
    minigl_raster_init();
}

static Texture *texture(uint32_t id)
{
    unsigned slot;
    unsigned probes;
    if (!id) return &g_default_texture;
    slot = (unsigned)(id * 2654435761U) & (MAX_TEXTURES * 2U - 1U);
    for (probes = 0U; probes < MAX_TEXTURES * 2U; ++probes) {
        uint16_t entry = g_texture_lookup[slot];
        if (!entry) return NULL;
        if (entry != UINT16_MAX && g_textures[entry - 1U].id == id)
            return &g_textures[entry - 1U];
        slot = (slot + 1U) & (MAX_TEXTURES * 2U - 1U);
    }
    return NULL;
}

static void texture_index(uint32_t id, uint16_t entry)
{
    unsigned slot = (unsigned)(id * 2654435761U) & (MAX_TEXTURES * 2U - 1U);
    unsigned probes;
    unsigned vacant = MAX_TEXTURES * 2U;
    for (probes = 0U; probes < MAX_TEXTURES * 2U; ++probes) {
        uint16_t current = g_texture_lookup[slot];
        if (!current) {
            if (entry) g_texture_lookup[vacant < MAX_TEXTURES * 2U ? vacant : slot] = entry;
            return;
        }
        if (current == UINT16_MAX) { if (vacant == MAX_TEXTURES * 2U) vacant = slot; }
        else if (g_textures[current - 1U].id == id) {
            g_texture_lookup[slot] = entry ? entry : UINT16_MAX;
            return;
        }
        slot = (slot + 1U) & (MAX_TEXTURES * 2U - 1U);
    }
    if (entry && vacant < MAX_TEXTURES * 2U) g_texture_lookup[vacant] = entry;
}

static void texture_initialize(Texture *texture_info, uint32_t id)
{
    memset(texture_info, 0, sizeof(*texture_info));
    texture_info->id = id;
    texture_info->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    texture_info->mag_filter = GL_LINEAR;
    texture_info->wrap_s = GL_REPEAT;
    texture_info->wrap_t = GL_REPEAT;
    texture_info->valid = 1U;
}

static Texture *texture_create(uint32_t id)
{
    Texture *existing = texture(id);
    unsigned index;

    if (existing)
        return existing;
    for (index = 0; index < MAX_TEXTURES; ++index) {
        if (!g_textures[index].valid) {
            texture_initialize(&g_textures[index], id);
            texture_index(id, (uint16_t)(index + 1U));
            return &g_textures[index];
        }
    }
    return NULL;
}

static void texture_unlink(TextureLevel *level)
{
    TextureLevel **at = &g_texture_allocations;
    while (*at && *at != level) at = &(*at)->next;
    if (*at) *at = level->next;
    level->next = NULL;
}

static int texture_pool_ensure(uint32_t minimum_bytes, uint32_t used_bytes)
{
    if (minimum_bytes <= g_buffers.texture_pool_bytes)
        return 1;
    if (!g_texture_pool_grow ||
        !g_texture_pool_grow(g_texture_pool_grow_context, minimum_bytes,
                             used_bytes, &g_buffers.texture_pool,
                             &g_buffers.texture_pool_bytes))
        return 0;
    g_private_texture_pool = g_buffers.texture_pool;
    g_private_texture_pool_bytes = g_buffers.texture_pool_bytes;
    return minimum_bytes <= g_buffers.texture_pool_bytes;
}

static int texture_alloc(TextureLevel *level, uint32_t size, uint32_t *out)
{
    TextureLevel *current;
    TextureLevel **insert;
    uint32_t at = 0U;
    uint32_t old_size = level->size;
    uint32_t minimum = g_texture_used - old_size + size;

    if (!texture_pool_ensure(minimum, g_texture_used) ||
        size > g_buffers.texture_pool_bytes) return 0;
    for (current = g_texture_allocations; current; current = current->next) {
        if (current == level) continue;
        if (size <= current->offset - at) break;
        at = current->offset + current->size;
    }
    texture_unlink(level);
    if (size > g_buffers.texture_pool_bytes - at) {
        at = 0U;
        for (current = g_texture_allocations; current; current = current->next) {
            if (current->offset != at)
                memmove(g_buffers.texture_pool + at, g_buffers.texture_pool + current->offset, current->size);
            current->offset = at;
            at += current->size;
        }
    }
    level->offset = at;
    level->size = size;
    insert = &g_texture_allocations;
    while (*insert && (*insert)->offset < at) insert = &(*insert)->next;
    level->next = *insert;
    *insert = level;
    g_texture_used = g_texture_used - old_size + size;
    *out = at;
    return 1;
}

static int mip_filter(uint16_t filter)
{
    return filter >= GL_NEAREST_MIPMAP_NEAREST && filter <= GL_LINEAR_MIPMAP_LINEAR;
}

static void texture_complete(Texture *texture_info)
{
    const TextureLevel *base = &texture_info->levels[0];
    unsigned level = 0U;
    uint16_t width = base->width;
    uint16_t height = base->height;
    texture_info->complete = base->size != 0U;
    while (width > 1U || height > 1U) {
        const TextureLevel *image;
        width = width > 1U ? width / 2U : 1U;
        height = height > 1U ? height / 2U : 1U;
        image = &texture_info->levels[++level];
        if (!image->size || image->width != width || image->height != height || image->format != base->format)
            texture_info->complete = 0U;
    }
    texture_info->last_level = (uint8_t)level;
}

static uint32_t unpack_texel(uint16_t format, const uint8_t *data, uint32_t index)
{
    uint16_t value;
    uint8_t luminance;

    switch (format) {
    case ZZ9K_MGL_TEXFMT_RGBA8888:
        return be32(data + (size_t)index * 4U);
    case ZZ9K_MGL_TEXFMT_RGB565:
        value = be16(data + (size_t)index * 2U);
        return rgba(expand5((uint8_t)((value >> 11) & 0x1fU)), expand6((uint8_t)((value >> 5) & 0x3fU)),
                    expand5((uint8_t)(value & 0x1fU)), 255U);
    case ZZ9K_MGL_TEXFMT_ARGB1555:
        value = be16(data + (size_t)index * 2U);
        return rgba(expand5((uint8_t)((value >> 10) & 0x1fU)), expand5((uint8_t)((value >> 5) & 0x1fU)),
                    expand5((uint8_t)(value & 0x1fU)), (value & 0x8000U) ? 255U : 0U);
    case ZZ9K_MGL_TEXFMT_RGB888:
        return rgba(data[(size_t)index * 3U], data[(size_t)index * 3U + 1U],
                    data[(size_t)index * 3U + 2U], 255U);
    case ZZ9K_MGL_TEXFMT_LUM8:
        luminance = data[index];
        return rgba(luminance, luminance, luminance, 255U);
    case ZZ9K_MGL_TEXFMT_LUM_A8:
        luminance = data[(size_t)index * 2U];
        return rgba(luminance, luminance, luminance, data[(size_t)index * 2U + 1U]);
    case ZZ9K_MGL_TEXFMT_P8:
        return be32(data + (size_t)data[1024U + index] * 4U);
    default:
        return 0U;
    }
}

static int wrap_coord(int value, uint16_t length, uint16_t wrap)
{
    if (wrap == GL_REPEAT) {
        if ((length & (length - 1U)) == 0U) return (int)((unsigned)value & (length - 1U));
        value %= (int)length;
        return value < 0 ? value + length : value;
    }
    return value < 0 ? 0 : value >= (int)length ? (int)length - 1 : value;
}

static inline uint32_t mix_texel(uint32_t a, uint32_t b, uint32_t c, uint32_t d, float x, float y)
{
    float ra = (float)(a >> 24), ga = (float)((a >> 16) & 0xffU), ba = (float)((a >> 8) & 0xffU), aa = (float)(a & 0xffU);
    float rb = (float)(b >> 24), gb = (float)((b >> 16) & 0xffU), bb = (float)((b >> 8) & 0xffU), ab = (float)(b & 0xffU);
    float rc = (float)(c >> 24), gc = (float)((c >> 16) & 0xffU), bc = (float)((c >> 8) & 0xffU), ac = (float)(c & 0xffU);
    float rd = (float)(d >> 24), gd = (float)((d >> 16) & 0xffU), bd = (float)((d >> 8) & 0xffU), ad = (float)(d & 0xffU);

    float r_top = ra + (rb - ra) * x, r_bot = rc + (rd - rc) * x;
    float g_top = ga + (gb - ga) * x, g_bot = gc + (gd - gc) * x;
    float b_top = ba + (bb - ba) * x, b_bot = bc + (bd - bc) * x;
    float a_top = aa + (ab - aa) * x, a_bot = ac + (ad - ac) * x;

    uint32_t r = (uint32_t)(r_top + (r_bot - r_top) * y + 0.5f);
    uint32_t g = (uint32_t)(g_top + (g_bot - g_top) * y + 0.5f);
    uint32_t bl = (uint32_t)(b_top + (b_bot - b_top) * y + 0.5f);
    uint32_t al = (uint32_t)(a_top + (a_bot - a_top) * y + 0.5f);

    return (r << 24) | (g << 16) | (bl << 8) | al;
}

typedef struct TextureGradient { float dudx, dvdx, dudy, dvdy; } TextureGradient;

static uint32_t sample_level(const Texture *texture_info, unsigned level, uint16_t filter, float u, float v)
{
    const TextureLevel *image = &texture_info->levels[level];
    const uint32_t *texels = (const uint32_t *)(g_buffers.texture_pool + image->offset);
    float sample_x;
    float sample_y;
    int x;
    int y;
    uint32_t x0, x1, y0, y1;
    if (texture_info->wrap_s == GL_REPEAT) u -= fast_floorf(u); else u = clamp01(u);
    if (texture_info->wrap_t == GL_REPEAT) v -= fast_floorf(v); else v = clamp01(v);
    if (filter != GL_LINEAR)
        return texels[(uint32_t)wrap_coord((int)(u * image->width), image->width, texture_info->wrap_s) +
                      (uint32_t)wrap_coord((int)(v * image->height), image->height, texture_info->wrap_t) * image->width];
    sample_x = u * image->width - 0.5f;
    sample_y = v * image->height - 0.5f;
    x = (int)fast_floorf(sample_x);
    y = (int)fast_floorf(sample_y);
    x0 = (uint32_t)wrap_coord(x, image->width, texture_info->wrap_s);
    x1 = (uint32_t)wrap_coord(x + 1, image->width, texture_info->wrap_s);
    y0 = (uint32_t)wrap_coord(y, image->height, texture_info->wrap_t) * image->width;
    y1 = (uint32_t)wrap_coord(y + 1, image->height, texture_info->wrap_t) * image->width;
    return mix_texel(texels[x0 + y0], texels[x1 + y0], texels[x0 + y1], texels[x1 + y1],
                     sample_x - (float)x, sample_y - (float)y);
}
static inline uint16_t base_min_filter(uint16_t filter)
{
    return (filter == GL_LINEAR || filter == GL_LINEAR_MIPMAP_NEAREST ||
            filter == GL_LINEAR_MIPMAP_LINEAR) ? GL_LINEAR : GL_NEAREST;
}

static uint32_t sample(const Texture *texture_info, float u, float v, const TextureGradient *gradient)
{
    uint16_t min_filter = texture_info->min_filter;
    uint16_t mag_filter = texture_info->mag_filter;
    float lambda;
    float rho2;
    float threshold;
    unsigned level;
    uint16_t filter;
    if (!mip_filter(min_filter) && min_filter == mag_filter)
        return sample_level(texture_info, 0U, min_filter, u, v);
    if (!gradient) {
        return sample_level(texture_info, 0U,
                            (!texture_info->complete && mip_filter(min_filter))
                                ? base_min_filter(min_filter) : mag_filter,
                            u, v);
    }
    {
        const TextureLevel *base = &texture_info->levels[0];
        float ux = gradient->dudx * base->width;
        float vx = gradient->dvdx * base->height;
        float uy = gradient->dudy * base->width;
        float vy = gradient->dvdy * base->height;
        rho2 = fmaxf(ux * ux + vx * vx, uy * uy + vy * vy);
    }
    threshold = mag_filter == GL_LINEAR &&
        (min_filter == GL_NEAREST_MIPMAP_NEAREST || min_filter == GL_NEAREST_MIPMAP_LINEAR) ? 0.5f : 0.0f;
    if (rho2 <= (threshold == 0.0f ? 1.0f : 2.0f))
        return sample_level(texture_info, 0U, mag_filter, u, v);
    if (!texture_info->complete)
        return sample_level(texture_info, 0U, base_min_filter(min_filter), u, v);
    if (!mip_filter(min_filter)) return sample_level(texture_info, 0U, min_filter, u, v);
    lambda = 0.5f * log2f(rho2);
    if (lambda > texture_info->last_level) lambda = texture_info->last_level;
    if (lambda < 0.0f) lambda = 0.0f;
    filter = (min_filter == GL_LINEAR_MIPMAP_NEAREST || min_filter == GL_LINEAR_MIPMAP_LINEAR) ? GL_LINEAR : GL_NEAREST;
    if (min_filter == GL_NEAREST_MIPMAP_NEAREST || min_filter == GL_LINEAR_MIPMAP_NEAREST) {
        level = (unsigned)(lambda + 0.5f);
        return sample_level(texture_info, level, filter, u, v);
    }
    level = (unsigned)lambda;
    {
        unsigned next = level < texture_info->last_level ? level + 1U : level;
        uint32_t a = sample_level(texture_info, level, filter, u, v);
        uint32_t b = sample_level(texture_info, next, filter, u, v);
        return mix_texel(a, b, a, b, lambda - level, 0.0f);
    }
}

static inline uint32_t modulate(uint32_t left, uint32_t right)
{
#if defined(__ARM_NEON)
    uint8x8_t vl = vreinterpret_u8_u32(vdup_n_u32(left));
    uint8x8_t vr = vreinterpret_u8_u32(vdup_n_u32(right));
    uint16x8_t prod = vmull_u8(vl, vr);
    uint8x8_t res = vrshrn_n_u16(prod, 8);
    return vget_lane_u32(vreinterpret_u32_u8(res), 0);
#else
    return rgba(mul8(channel(left, 24U), channel(right, 24U)),
                mul8(channel(left, 16U), channel(right, 16U)),
                mul8(channel(left, 8U), channel(right, 8U)),
                mul8(channel(left, 0U), channel(right, 0U)));
#endif
}

static uint32_t texture_env(uint16_t mode, uint32_t incoming, uint32_t texel, uint32_t env_color, int internal_rgb)
{
    uint8_t alpha = channel(incoming, 0U);
    uint8_t texel_alpha = channel(texel, 0U);
    switch (mode) {
    case GL_MODULATE: return modulate(incoming, texel);
    case GL_REPLACE: return internal_rgb ? (texel & 0xffffff00U) | channel(incoming, 0U) : texel;
    case GL_DECAL:
        return rgba((uint8_t)(mul8(channel(texel, 24U), texel_alpha) + mul8(channel(incoming, 24U), (uint8_t)(255U - texel_alpha))),
                    (uint8_t)(mul8(channel(texel, 16U), texel_alpha) + mul8(channel(incoming, 16U), (uint8_t)(255U - texel_alpha))),
                    (uint8_t)(mul8(channel(texel, 8U), texel_alpha) + mul8(channel(incoming, 8U), (uint8_t)(255U - texel_alpha))), alpha);
    case GL_TEXTURE_ENV_BLEND:
        return rgba((uint8_t)(mul8(channel(incoming, 24U), (uint8_t)(255U - channel(texel, 24U))) + mul8(channel(env_color, 24U), channel(texel, 24U))),
                    (uint8_t)(mul8(channel(incoming, 16U), (uint8_t)(255U - channel(texel, 16U))) + mul8(channel(env_color, 16U), channel(texel, 16U))),
                    (uint8_t)(mul8(channel(incoming, 8U), (uint8_t)(255U - channel(texel, 8U))) + mul8(channel(env_color, 8U), channel(texel, 8U))), mul8(alpha, texel_alpha));
    case GL_ADD:
        return rgba(satadd(channel(incoming, 24U), channel(texel, 24U)), satadd(channel(incoming, 16U), channel(texel, 16U)),
                    satadd(channel(incoming, 8U), channel(texel, 8U)), mul8(alpha, texel_alpha));
    default: g_error |= RASTER_ERR_STATE; return incoming;
    }
}

static uint32_t apply_textures(float u0, float v0, float u1, float v1, uint32_t color,
                               const TextureGradient *gradients)
{
    Texture *texture_info;
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) {
        texture_info = g_bound_texture_info[0];
        if (texture_info && texture_info->levels[0].size)
            color = texture_env(g_state.tex_env_mode[0], color, sample(texture_info, u0, v0, gradients ? &gradients[0] : NULL), g_state.tex_env_color[0], texture_info->levels[0].internal_rgb);
    }
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) {
        texture_info = g_bound_texture_info[1];
        if (texture_info && texture_info->levels[0].size)
            color = texture_env(g_state.tex_env_mode[1], color, sample(texture_info, u1, v1, gradients ? &gradients[1] : NULL), g_state.tex_env_color[1], texture_info->levels[0].internal_rgb);
    }
    return color;
}

static uint32_t depth_get(uint32_t index)
{
    const uint8_t *value = g_buffers.depth + index * depth_bytes_per_pixel();
    return g_depth_bits == 16U ? ((uint32_t)value[0] << 8) | value[1] :
        ((uint32_t)value[0] << 16) | ((uint32_t)value[1] << 8) | value[2];
}

static void depth_put(uint32_t index, uint32_t value)
{
    uint8_t *out = g_buffers.depth + index * depth_bytes_per_pixel();
    if (g_depth_bits == 16U) {
        out[0] = (uint8_t)(value >> 8);
        out[1] = (uint8_t)value;
    } else {
        out[0] = (uint8_t)(value >> 16);
        out[1] = (uint8_t)(value >> 8);
        out[2] = (uint8_t)value;
    }
}

static int compare(uint32_t function, uint32_t left, uint32_t right)
{
    switch (function ? function : GL_LESS) {
    case GL_NEVER: return 0;
    case GL_LESS: return left < right;
    case GL_EQUAL: return left == right;
    case GL_LEQUAL: return left <= right;
    case GL_GREATER: return left > right;
    case GL_NOTEQUAL: return left != right;
    case GL_GEQUAL: return left >= right;
    case GL_ALWAYS: return 1;
    default: return 0;
    }
}

static uint8_t factor(uint16_t function, uint8_t source, uint8_t destination,
                      uint8_t source_alpha, uint8_t destination_alpha)
{
    switch (function) {
    case GL_ZERO: return 0U;
    case GL_ONE: return 255U;
    case GL_SRC_ALPHA: return source_alpha;
    case GL_ONE_MINUS_SRC_ALPHA: return (uint8_t)(255U - source_alpha);
    case GL_DST_ALPHA: return destination_alpha;
    case GL_ONE_MINUS_DST_ALPHA: return (uint8_t)(255U - destination_alpha);
    case GL_SRC_COLOR: return source;
    case GL_ONE_MINUS_SRC_COLOR: return (uint8_t)(255U - source);
    case GL_DST_COLOR: return destination;
    case GL_ONE_MINUS_DST_COLOR: return (uint8_t)(255U - destination);
    case GL_SRC_ALPHA_SATURATE: {
        uint8_t inv_dst = (uint8_t)(255U - destination_alpha);
        return source_alpha < inv_dst ? source_alpha : inv_dst;
    }
    default: return 0U;
    }
}

static uint8_t blend_equation(uint16_t equation, uint8_t source, uint8_t destination,
                              uint8_t source_factor, uint8_t destination_factor)
{
    switch (equation) {
    case GL_FUNC_ADD:
        return satadd(mul8(source, source_factor), mul8(destination, destination_factor));
    case GL_MIN:
        return source < destination ? source : destination;
    case GL_MAX:
        return source > destination ? source : destination;
    case GL_FUNC_SUBTRACT: {
        int value = (int)mul8(source, source_factor) - (int)mul8(destination, destination_factor);
        return value > 0 ? (uint8_t)value : 0U;
    }
    case GL_FUNC_REVERSE_SUBTRACT: {
        int value = (int)mul8(destination, destination_factor) - (int)mul8(source, source_factor);
        return value > 0 ? (uint8_t)value : 0U;
    }
    default:
        return 0U;
    }
}

static uint32_t blend(uint32_t source, uint32_t destination)
{
    const unsigned shifts[3] = {24U, 16U, 8U};
    uint8_t out[4];
    uint8_t source_alpha = channel(source, 0U);
    uint8_t destination_alpha = channel(destination, 0U);
    unsigned index;
    if (g_state.blend_equation == GL_MIN || g_state.blend_equation == GL_MAX) {
        for (index = 0U; index < 3U; ++index) {
            uint8_t source_channel = channel(source, shifts[index]);
            uint8_t destination_channel = channel(destination, shifts[index]);
            out[index] = g_state.blend_equation == GL_MIN ?
                (source_channel < destination_channel ? source_channel : destination_channel) :
                (source_channel > destination_channel ? source_channel : destination_channel);
        }
        out[3] = g_state.blend_equation == GL_MIN ?
            (source_alpha < destination_alpha ? source_alpha : destination_alpha) :
            (source_alpha > destination_alpha ? source_alpha : destination_alpha);
        return rgba(out[0], out[1], out[2], out[3]);
    }

    for (index = 0U; index < 3U; ++index) {
        uint8_t source_channel = channel(source, shifts[index]);
        uint8_t destination_channel = channel(destination, shifts[index]);
        out[index] = blend_equation(g_state.blend_equation, source_channel, destination_channel,
                                    factor(g_state.blend_src, source_channel, destination_channel,
                                           source_alpha, destination_alpha),
                                    factor(g_state.blend_dst, source_channel, destination_channel,
                                           source_alpha, destination_alpha));
    }
    out[3] = blend_equation(g_state.blend_equation, source_alpha, destination_alpha,
                            g_state.blend_src_alpha == GL_SRC_ALPHA_SATURATE ? 255U :
                                factor(g_state.blend_src_alpha, source_alpha, destination_alpha,
                                       source_alpha, destination_alpha),
                            factor(g_state.blend_dst_alpha, source_alpha, destination_alpha,
                                   source_alpha, destination_alpha));
    return rgba(out[0], out[1], out[2], out[3]);
}

static uint32_t apply_fog(uint32_t color, float eye_distance)
{
    float factor_value;
    uint32_t fog_color;

    if (!(g_state.enabled_flags & ZZ9K_MGL_STATE_FOG))
        return color;
    eye_distance = fabsf(eye_distance);
    if (g_state.fog_mode == GL_EXP) {
        factor_value = expf(-g_state.fog_density * eye_distance);
    } else if (g_state.fog_mode == GL_EXP2) {
        float density_distance = g_state.fog_density * eye_distance;
        factor_value = expf(-(density_distance * density_distance));
    } else {
        factor_value = g_state.fog_end == g_state.fog_start ?
            (eye_distance < g_state.fog_end ? 1.0f : 0.0f) :
            (g_state.fog_end - eye_distance) / (g_state.fog_end - g_state.fog_start);
    }
    factor_value = clamp01(factor_value);
    fog_color = g_state.fog_color;
    float r = (float)channel(fog_color, 24U) + ((float)channel(color, 24U) - (float)channel(fog_color, 24U)) * factor_value;
    float g = (float)channel(fog_color, 16U) + ((float)channel(color, 16U) - (float)channel(fog_color, 16U)) * factor_value;
    float b = (float)channel(fog_color, 8U) + ((float)channel(color, 8U) - (float)channel(fog_color, 8U)) * factor_value;
    return rgba((uint8_t)(clamp01(r / 255.0f) * 255.0f + 0.5f),
                (uint8_t)(clamp01(g / 255.0f) * 255.0f + 0.5f),
                (uint8_t)(clamp01(b / 255.0f) * 255.0f + 0.5f),
                channel(color, 0U));
}

static uint32_t mask_color(uint32_t old, uint32_t value)
{
    uint32_t mask = g_state.color_mask & 0xfU;
    if (!(mask & 1U)) value = (value & 0x00ffffffU) | (old & 0xff000000U);
    if (!(mask & 2U)) value = (value & 0xff00ffffU) | (old & 0x00ff0000U);
    if (!(mask & 4U)) value = (value & 0xffff00ffU) | (old & 0x0000ff00U);
    if (!(mask & 8U)) value = (value & 0xffffff00U) | (old & 0x000000ffU);
    return value;
}

static void fragment(int x, int y, float z, float u0, float v0, float u1, float v1,
                     float fog_coord, uint32_t color, const TextureGradient *gradients,
                     float polygon_offset)
{
    uint32_t index;
    uint32_t depth_value;
    float ranged_depth;

    if (x < 0 || y < 0 || x >= g_buffers.width || y >= g_buffers.height ||
        z < 0.0f || z > 1.0f)
        return;
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_SCISSOR_TEST) {
        int32_t left = g_state.scissor_x;
        int32_t top = (int32_t)g_buffers.height - g_state.scissor_y - g_state.scissor_h;
        if (x < left || x >= left + g_state.scissor_w ||
            y < top || y >= top + g_state.scissor_h)
            return;
    }
    index = (uint32_t)y * g_buffers.width + (uint32_t)x;
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_DEPTH_TEST) {
        if (g_state.depth_offset == 0.0f && polygon_offset == 0.0f && (z == 0.0f || z == 1.0f)) {
            uint32_t endpoint = z == 0.0f ? g_state.depth_near : g_state.depth_far;
            if (endpoint > 0x00ffffffU) endpoint = 0x00ffffffU;
            depth_value = g_depth_bits == 24U ? endpoint :
                (uint32_t)((double)endpoint * 65535.0 / 16777215.0 + 0.5);
        } else {
            ranged_depth = ((float)g_state.depth_near + z * ((float)g_state.depth_far - g_state.depth_near)) / 16777215.0f;
            ranged_depth = clamp01(ranged_depth + g_state.depth_offset + polygon_offset);
            depth_value = (uint32_t)(ranged_depth * depth_max() + 0.5f);
            if (depth_value > depth_max()) depth_value = depth_max();
        }
        if (!compare(g_state.depth_func, depth_value, depth_get(index)))
            return;
    } else {
        depth_value = 0;
    }
    color = apply_textures(u0, v0, u1, v1, color, gradients);
    if ((g_state.enabled_flags & ZZ9K_MGL_STATE_ALPHA_TEST) &&
        !compare(g_state.alpha_func ? g_state.alpha_func : GL_ALWAYS, channel(color, 0U),
                 g_state.alpha_ref > 255U ? g_state.alpha_ref >> 8 : g_state.alpha_ref))
        return;
    color = apply_fog(color, fog_coord);
    if ((g_state.enabled_flags & (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE)) ==
        (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE))
        depth_put(index, depth_value);
    /* Masking color output must not bypass alpha rejection or depth writes. */
    if ((g_state.color_mask & 0xfU) == 0U)
        return;
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_BLEND)
        color = blend(color, g_buffers.color[index]);
    if ((g_state.color_mask & 0xfU) == 0xfU)
        g_buffers.color[index] = color;
    else
        g_buffers.color[index] = mask_color(g_buffers.color[index], color);
}

static uint32_t color_lerp(uint32_t a, uint32_t b, float amount)
{
    return rgba((uint8_t)(channel(a, 24U) + (channel(b, 24U) - channel(a, 24U)) * amount),
                (uint8_t)(channel(a, 16U) + (channel(b, 16U) - channel(a, 16U)) * amount),
                (uint8_t)(channel(a, 8U) + (channel(b, 8U) - channel(a, 8U)) * amount),
                (uint8_t)(channel(a, 0U) + (channel(b, 0U) - channel(a, 0U)) * amount));
}

static Vertex lerp_vertex(const Vertex *a, const Vertex *b, float amount)
{
    Vertex out;

    out.x = a->x + (b->x - a->x) * amount;
    out.y = a->y + (b->y - a->y) * amount;
    out.z = a->z + (b->z - a->z) * amount;
    out.w = a->w + (b->w - a->w) * amount;
    out.inv_w = 0.0f;
    out.sx = out.sy = out.sz = 0.0f;
    out.u0 = a->u0 + (b->u0 - a->u0) * amount;
    out.v0 = a->v0 + (b->v0 - a->v0) * amount;
    out.q0 = a->q0 + (b->q0 - a->q0) * amount;
    out.u1 = a->u1 + (b->u1 - a->u1) * amount;
    out.v1 = a->v1 + (b->v1 - a->v1) * amount;
    out.q1 = a->q1 + (b->q1 - a->q1) * amount;
    out.fog_coord = a->fog_coord + (b->fog_coord - a->fog_coord) * amount;
    out.color = color_lerp(a->color, b->color, amount);
    out.front_color = color_lerp(a->front_color, b->front_color, amount);
    out.back_color = color_lerp(a->back_color, b->back_color, amount);
    out.flat_color = a->flat_color;
    return out;
}

static float plane(const Vertex *vertex, unsigned index)
{
    switch (index) {
    case 0U: return vertex->x + vertex->w;
    case 1U: return vertex->w - vertex->x;
    case 2U: return vertex->y + vertex->w;
    case 3U: return vertex->w - vertex->y;
    case 4U: return vertex->z + vertex->w;
    default: return vertex->w - vertex->z;
    }
}

static unsigned clip_polygon(const Vertex *in, unsigned count, Vertex *out)
{
    Vertex first[CLIP_MAX_VERTICES];
    Vertex second[CLIP_MAX_VERTICES];
    unsigned plane_index;
    unsigned index;

    if (count < 2U || count + 6U > CLIP_MAX_VERTICES) {
        g_error |= RASTER_ERR_MEMORY;
        return 0U;
    }
    {
        unsigned any = 0U;
        unsigned all = 63U;
        for (index = 0U; index < count; ++index) {
            unsigned code = 0U;
            for (plane_index = 0U; plane_index < 6U; ++plane_index)
                if (plane(&in[index], plane_index) < 0.0f) code |= 1U << plane_index;
            any |= code;
            all &= code;
        }
        if (all) return 0U;
        if (!any) { memcpy(out, in, count * sizeof(*out)); return count; }
    }
    memcpy(first, in, count * sizeof(*in));
    for (plane_index = 0U; plane_index < 6U; ++plane_index) {
        unsigned result_count = 0U;
        for (index = 0U; index < count; ++index) {
            const Vertex *start = &first[index];
            const Vertex *end = &first[(index + 1U) % count];
            float start_distance = plane(start, plane_index);
            float end_distance = plane(end, plane_index);
            int start_inside = start_distance >= 0.0f;
            int end_inside = end_distance >= 0.0f;

            if (start_inside) {
                if (result_count >= CLIP_MAX_VERTICES) {
                    g_error |= RASTER_ERR_MEMORY;
                    return 0U;
                }
                second[result_count++] = *start;
            }
            if (start_inside != end_inside) {
                if (result_count >= CLIP_MAX_VERTICES) {
                    g_error |= RASTER_ERR_MEMORY;
                    return 0U;
                }
                second[result_count++] = lerp_vertex(start, end, start_distance / (start_distance - end_distance));
            }
        }
        memcpy(first, second, result_count * sizeof(*first));
        count = result_count;
        if (!count)
            break;
    }
    memcpy(out, first, count * sizeof(*out));
    return count;
}

static int project(Vertex *vertex)
{
    float inverse_w;
    float gl_y;

    if (vertex->w == 0.0f)
        return 0;
    inverse_w = 1.0f / vertex->w;
    vertex->inv_w = inverse_w;
    vertex->sx = (vertex->x * inverse_w + 1.0f) * 0.5f * g_state.viewport_w + g_state.viewport_x;
    gl_y = (vertex->y * inverse_w + 1.0f) * 0.5f * g_state.viewport_h + g_state.viewport_y;
    vertex->sy = (float)g_buffers.height - gl_y;
    vertex->sz = vertex->z * inverse_w * 0.5f + 0.5f;
    return 1;
}

static void texture_matrix(unsigned unit, float *u, float *v, float r, float *q)
{
    const float *matrix = g_matrix[2U + unit];
    float input_u = *u;
    float input_v = *v;
    float input_r = r;
    float input_q = *q;

    *u = input_u * matrix[0] + input_v * matrix[4] + input_r * matrix[8] + input_q * matrix[12];
    *v = input_u * matrix[1] + input_v * matrix[5] + input_r * matrix[9] + input_q * matrix[13];
    *q = input_u * matrix[3] + input_v * matrix[7] + input_r * matrix[11] + input_q * matrix[15];
}

#if defined(__ARM_NEON)
static int neon_input_ok(float x, float y, float z, float w)
{
    return isfinite(x) && isfinite(y) && isfinite(z) && isfinite(w) &&
           fpclassify(x) != FP_SUBNORMAL && fpclassify(y) != FP_SUBNORMAL &&
           fpclassify(z) != FP_SUBNORMAL && fpclassify(w) != FP_SUBNORMAL;
}
#endif

static void transform_mvp(const float *matrix, float x, float y, float z, float w, float out[4])
{
#if defined(__ARM_NEON)
    if (g_mvp_neon_ok && neon_input_ok(x, y, z, w)) {
        float32x4_t value = vmulq_n_f32(vld1q_f32(matrix), x);
        value = vmlaq_n_f32(value, vld1q_f32(matrix + 4U), y);
        value = vmlaq_n_f32(value, vld1q_f32(matrix + 8U), z);
        value = vmlaq_n_f32(value, vld1q_f32(matrix + 12U), w);
        vst1q_f32(out, value);
        return;
    }
#endif
    out[0] = x * matrix[0] + y * matrix[4] + z * matrix[8] + w * matrix[12];
    out[1] = x * matrix[1] + y * matrix[5] + z * matrix[9] + w * matrix[13];
    out[2] = x * matrix[2] + y * matrix[6] + z * matrix[10] + w * matrix[14];
    out[3] = x * matrix[3] + y * matrix[7] + z * matrix[11] + w * matrix[15];
}

static int read_vertex(const uint8_t *source, uint16_t stride, Vertex *out)
{
    const float *modelview = g_matrix[0];
    const float *mvp = g_mvp;
    float x;
    float y;
    float z;

    if (stride != MINIGL_RASTER_VERTEX_STRIDE)
        return 0;
    if (g_trace_hook != NULL)
        g_trace_hook(1U);
    if (g_draw_attr_code & MINIGL_RASTER_ATTR_POSITION_INT32) {
        x = (float)(int32_t)be32(source);
        y = (float)(int32_t)be32(source + 4U);
        z = (float)(int32_t)be32(source + 8U);
    } else {
#if defined(__ARM_NEON)
        {
            float position[4];
            be_float4(source, position);
            x = position[0];
            y = position[1];
            z = position[2];
        }
#else
        x = bef(source);
        y = bef(source + 4U);
        z = bef(source + 8U);
#endif
    }
    {
        float input_w = bef(source + 44U);
        float eye_position[4];
        float clip_position[4];
        int lighting = minigl_lighting_enabled();
        int need_eye_position = lighting || (g_state.enabled_flags & ZZ9K_MGL_STATE_FOG);

        transform_mvp(mvp, x, y, z, input_w, clip_position);
        out->x = clip_position[0];
        out->y = clip_position[1];
        out->z = clip_position[2];
        out->w = clip_position[3];
        out->color = be32(source + 12U);
        if (need_eye_position) {
            eye_position[0] = x * modelview[0] + y * modelview[4] + z * modelview[8] + input_w * modelview[12];
            eye_position[1] = x * modelview[1] + y * modelview[5] + z * modelview[9] + input_w * modelview[13];
            eye_position[2] = x * modelview[2] + y * modelview[6] + z * modelview[10] + input_w * modelview[14];
            eye_position[3] = x * modelview[3] + y * modelview[7] + z * modelview[11] + input_w * modelview[15];
            out->fog_coord = eye_position[2];
        } else {
            out->fog_coord = 0.0f;
        }
        if (lighting) {
            float normal[3] = {bef(source + 32U), bef(source + 36U), bef(source + 40U)};
            minigl_lighting_shade(eye_position, normal, out->color, &out->front_color, &out->back_color);
        } else {
            out->front_color = out->color;
            out->back_color = out->color;
        }
        out->color = out->front_color;
        out->flat_color = out->front_color;
    }
    {
        float r0 = bef(source + 56U);
        float r1 = bef(source + 60U);
        float q0 = bef(source + 48U);
        float q1 = bef(source + 52U);
        out->u0 = bef(source + 16U);
        out->v0 = bef(source + 20U);
        out->q0 = q0;
        out->u1 = bef(source + 24U);
        out->v1 = bef(source + 28U);
        out->q1 = q1;
        if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0)
            texture_matrix(0U, &out->u0, &out->v0, r0, &out->q0);
        if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1)
            texture_matrix(1U, &out->u1, &out->v1, r1, &out->q1);
    }
    if (g_trace_hook != NULL)
        g_trace_hook(0U);
    return 1;
}

static int vertex_address(uint32_t base, uint32_t index, uint16_t stride, const uint8_t **out)
{
    uint64_t address = (uint64_t)base + (uint64_t)index * stride;

    if (stride != MINIGL_RASTER_VERTEX_STRIDE || address + stride > g_buffers.vertices_bytes)
        return 0;
    *out = g_buffers.vertices + (uint32_t)address;
    return 1;
}

static int fetch(uint16_t stride, uint32_t base, uint32_t index, Vertex *out)
{
    const uint8_t *source;

    if (!vertex_address(base, index, stride, &source) || !read_vertex(source, stride, out)) {
        g_error |= RASTER_ERR_VERTEX;
        return 0;
    }
    return 1;
}

static uint32_t interp_color(const Vertex *a, const Vertex *b, const Vertex *c,
                             float wa, float wb, float wc, float denominator)
{
    const unsigned shifts[4] = {24U, 16U, 8U, 0U};
    uint8_t out[4];
    unsigned index;

    if (g_state.shade_model == GL_FLAT)
        return a->flat_color;
    for (index = 0U; index < 4U; ++index) {
        float value = (channel(a->color, shifts[index]) * wa * a->inv_w +
                       channel(b->color, shifts[index]) * wb * b->inv_w +
                       channel(c->color, shifts[index]) * wc * c->inv_w) / denominator;
        out[index] = (uint8_t)(clamp01(value / 255.0f) * 255.0f + 0.5f);
    }
    return rgba(out[0], out[1], out[2], out[3]);
}

static float edge(const Vertex *a, const Vertex *b, float x, float y)
{
    return (b->sx - a->sx) * (y - a->sy) - (b->sy - a->sy) * (x - a->sx);
}

#define RAST_SUBPIX_SHIFT 8
#define RAST_SUBPIX_ONE   (1 << RAST_SUBPIX_SHIFT)
#define RAST_SUBPIX_HALF  (RAST_SUBPIX_ONE / 2)

typedef struct RastPoint {
    int32_t x;
    int32_t y;
} RastPoint;

static inline RastPoint rast_to_fixed(float sx, float sy)
{
    RastPoint p;
    p.x = (int32_t)roundf(sx * (float)RAST_SUBPIX_ONE);
    p.y = (int32_t)roundf(sy * (float)RAST_SUBPIX_ONE);
    return p;
}

static inline int rast_top_left(RastPoint a, RastPoint b)
{
    int32_t dx = b.x - a.x;
    int32_t dy = b.y - a.y;
    return dy < 0 || (dy == 0 && dx > 0);
}

static inline int rast_edge_covers(RastPoint a, RastPoint b, int x, int y, int inclusive)
{
    int64_t px = ((int64_t)x << RAST_SUBPIX_SHIFT) + RAST_SUBPIX_HALF;
    int64_t py = ((int64_t)y << RAST_SUBPIX_SHIFT) + RAST_SUBPIX_HALF;
    int64_t e = (int64_t)(b.x - a.x) * (py - (int64_t)a.y) - (int64_t)(b.y - a.y) * (px - (int64_t)a.x);
    return e > 0 || (e == 0 && inclusive);
}

static void edge_span(RastPoint a, RastPoint b, int y, int *left, int *right)
{
    int lo = *left;
    int hi = *right;
    int inclusive = rast_top_left(a, b);
    int descending = b.y > a.y;
    if (lo > hi) return;
    if (b.y == a.y) {
        if (!rast_edge_covers(a, b, lo, y, inclusive)) *right = lo - 1;
        return;
    }
    if (descending) {
        if (!rast_edge_covers(a, b, lo, y, inclusive)) { *right = lo - 1; return; }
        if (rast_edge_covers(a, b, hi, y, inclusive)) return;
        while (lo + 1 < hi) {
            int mid = lo + (hi - lo) / 2;
            if (rast_edge_covers(a, b, mid, y, inclusive)) lo = mid;
            else hi = mid;
        }
        *right = lo;
    } else {
        if (!rast_edge_covers(a, b, hi, y, inclusive)) { *right = lo - 1; return; }
        if (rast_edge_covers(a, b, lo, y, inclusive)) return;
        while (lo + 1 < hi) {
            int mid = lo + (hi - lo) / 2;
            if (rast_edge_covers(a, b, mid, y, inclusive)) hi = mid;
            else lo = mid;
        }
        *left = hi;
    }
}

typedef struct TextureSlope { float ux, vx, qx, uy, vy, qy; } TextureSlope;

static unsigned gradient_units(void)
{
    unsigned unit;
    unsigned mask = 0U;
    for (unit = 0U; unit < MAX_TMU; ++unit) {
        Texture *image = g_bound_texture_info[unit];
        if ((g_state.enabled_flags & (unit ? ZZ9K_MGL_STATE_TEXTURE_2D_1 : ZZ9K_MGL_STATE_TEXTURE_2D_0)) &&
            image && image->levels[0].size && image->complete &&
            (mip_filter(image->min_filter) || image->min_filter != image->mag_filter)) mask |= 1U << unit;
    }
    return mask;
}

static TextureSlope texture_slope(const Vertex *a, const Vertex *b, const Vertex *c, unsigned unit,
                                  float ax, float bx, float cx, float ay, float by, float cy)
{
    TextureSlope out;
    float au = unit ? a->u1 : a->u0, bu = unit ? b->u1 : b->u0, cu = unit ? c->u1 : c->u0;
    float av = unit ? a->v1 : a->v0, bv = unit ? b->v1 : b->v0, cv = unit ? c->v1 : c->v0;
    float aq = unit ? a->q1 : a->q0, bq = unit ? b->q1 : b->q0, cq = unit ? c->q1 : c->q0;
    out.ux = ax * au * a->inv_w + bx * bu * b->inv_w + cx * cu * c->inv_w;
    out.vx = ax * av * a->inv_w + bx * bv * b->inv_w + cx * cv * c->inv_w;
    out.qx = ax * aq * a->inv_w + bx * bq * b->inv_w + cx * cq * c->inv_w;
    out.uy = ay * au * a->inv_w + by * bu * b->inv_w + cy * cu * c->inv_w;
    out.vy = ay * av * a->inv_w + by * bv * b->inv_w + cy * cv * c->inv_w;
    out.qy = ay * aq * a->inv_w + by * bq * b->inv_w + cy * cq * c->inv_w;
    return out;
}

static TextureGradient texture_gradient(const TextureSlope *slope, float u, float v, float q)
{
    TextureGradient out;
    out.dudx = (slope->ux - u * slope->qx) / q;
    out.dvdx = (slope->vx - v * slope->qx) / q;
    out.dudy = (slope->uy - u * slope->qy) / q;
    out.dvdy = (slope->vy - v * slope->qy) / q;
    return out;
}

static uint32_t polygon_offset_flag(uint16_t mode)
{
    switch (mode) {
    case GL_FILL: return ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
    case GL_LINE: return ZZ9K_MGL_STATE_POLYGON_OFFSET_LINE;
    case GL_POINT: return ZZ9K_MGL_STATE_POLYGON_OFFSET_POINT;
    default: return 0U;
    }
}

static float polygon_offset_bias(const Vertex *a, const Vertex *b, const Vertex *c)
{
    uint16_t mode = g_state.polygon_mode ? g_state.polygon_mode : GL_FILL;
    uint32_t flag = polygon_offset_flag(mode);
    float units;
    float area;
    float scale;
    float za;
    float zb;
    float zc;
    float dzdx;
    float dzdy;

    if (!flag || !(g_state.enabled_flags & flag) ||
        (g_state.polygon_offset_factor == 0.0f && g_state.polygon_offset_units == 0.0f))
        return 0.0f;
    units = g_polygon_offset_units;
    if (g_state.polygon_offset_factor == 0.0f)
        return units;
    area = edge(a, b, c->sx, c->sy);
    if (area == 0.0f)
        return units;
    scale = g_polygon_depth_scale;
    za = a->sz * scale;
    zb = b->sz * scale;
    zc = c->sz * scale;
    dzdx = ((zb - za) * (c->sy - a->sy) - (zc - za) * (b->sy - a->sy)) / area;
    dzdy = ((b->sx - a->sx) * (zc - za) - (c->sx - a->sx) * (zb - za)) / area;
    return g_state.polygon_offset_factor * fmaxf(fabsf(dzdx), fabsf(dzdy)) + units;
}

static float polygon_offset_polygon(const Vertex *vertices, unsigned count)
{
    unsigned index;
    uint16_t mode = g_state.polygon_mode ? g_state.polygon_mode : GL_FILL;
    uint32_t flag = polygon_offset_flag(mode);

    if (!flag || !(g_state.enabled_flags & flag) ||
        (g_state.polygon_offset_factor == 0.0f && g_state.polygon_offset_units == 0.0f))
        return 0.0f;
    if (g_state.polygon_offset_factor == 0.0f)
        return g_polygon_offset_units;
    for (index = 2U; index < count; ++index)
        if (edge(&vertices[0], &vertices[index - 1U], vertices[index].sx, vertices[index].sy) != 0.0f)
            return polygon_offset_bias(&vertices[0], &vertices[index - 1U], &vertices[index]);
    return g_polygon_offset_units;
}
static inline int clamp_coord(int value, int max_val)
{
    return value < 0 ? 0 : value > max_val ? max_val : value;
}

static inline uint32_t sample_pot_linear_repeat(const uint32_t *texels, float u, float v,
                                                float fw, float fh, uint32_t mask_x, uint32_t mask_y, unsigned shift_y)
{
    u -= fast_floorf(u);
    v -= fast_floorf(v);
    float sx = u * fw - 0.5f;
    float sy = v * fh - 0.5f;
    int ix = (int)fast_floorf(sx);
    int iy = (int)fast_floorf(sy);
    uint32_t x0 = (uint32_t)ix & mask_x;
    uint32_t x1 = (uint32_t)(ix + 1) & mask_x;
    uint32_t y0 = ((uint32_t)iy & mask_y) << shift_y;
    uint32_t y1 = ((uint32_t)(iy + 1) & mask_y) << shift_y;
    return mix_texel(texels[x0 + y0], texels[x1 + y0], texels[x0 + y1], texels[x1 + y1],
                     sx - (float)ix, sy - (float)iy);
}

static inline uint32_t sample_pot_nearest_clamp(const uint32_t *texels, float u, float v,
                                                float fw, float fh, int max_x, int max_y, unsigned shift_y)
{
    u = clamp01(u);
    v = clamp01(v);
    int ix = (int)(u * fw);
    int iy = (int)(v * fh);
    uint32_t x = (uint32_t)clamp_coord(ix, max_x);
    uint32_t y = ((uint32_t)clamp_coord(iy, max_y)) << shift_y;
    return texels[x + y];
}

static inline uint32_t sample_pot_linear_clamp(const uint32_t *texels, float u, float v,
                                               float fw, float fh, int max_x, int max_y, unsigned shift_y)
{
    u = clamp01(u);
    v = clamp01(v);
    float sx = u * fw - 0.5f;
    float sy = v * fh - 0.5f;
    int ix = (int)fast_floorf(sx);
    int iy = (int)fast_floorf(sy);
    uint32_t x0 = (uint32_t)clamp_coord(ix, max_x);
    uint32_t x1 = (uint32_t)clamp_coord(ix + 1, max_x);
    uint32_t y0 = (uint32_t)clamp_coord(iy, max_y) << shift_y;
    uint32_t y1 = (uint32_t)clamp_coord(iy + 1, max_y) << shift_y;
    return mix_texel(texels[x0 + y0], texels[x1 + y0], texels[x0 + y1], texels[x1 + y1],
                     sx - (float)ix, sy - (float)iy);
}

static inline uint32_t blend_alpha_fast(uint32_t src, uint32_t dst, uint8_t a)
{
#if defined(__ARM_NEON)
    uint8x8_t vsrc = vreinterpret_u8_u32(vdup_n_u32(src));
    uint8x8_t vdst = vreinterpret_u8_u32(vdup_n_u32(dst));
    uint8x8_t va = vdup_n_u8(a);
    uint8x8_t vinv_a = vdup_n_u8((uint8_t)(255U - a));
    uint16x8_t p1 = vmull_u8(vsrc, va);
    uint16x8_t p2 = vmlal_u8(p1, vdst, vinv_a);
    uint8x8_t res = vrshrn_n_u16(p2, 8);
    return vget_lane_u32(vreinterpret_u32_u8(res), 0);
#else
    uint8_t inv_a = (uint8_t)(255U - a);
    uint32_t r = mul8((uint8_t)(src >> 24), a) + mul8((uint8_t)(dst >> 24), inv_a);
    uint32_t g = mul8((uint8_t)(src >> 16), a) + mul8((uint8_t)(dst >> 16), inv_a);
    uint32_t b = mul8((uint8_t)(src >> 8), a) + mul8((uint8_t)(dst >> 8), inv_a);
    uint32_t out_a = mul8(a, a) + mul8((uint8_t)(dst & 0xffU), inv_a);
    return (r << 24) | (g << 16) | (b << 8) | out_a;
#endif
}

static void triangle_fill(const Vertex *first, const Vertex *second, const Vertex *third, float polygon_offset)
{
    Vertex a = *first;
    Vertex b = *second;
    Vertex c = *third;
    float area = edge(&a, &b, c.sx, c.sy);
    int x0;
    int x1;
    int y0;
    int y1;
    int x;
    int y;
    int endpoint_depth = (a.sz == 0.0f || a.sz == 1.0f) && a.sz == b.sz && a.sz == c.sz;
    unsigned units = gradient_units();
    unsigned unit;
    TextureSlope slopes[MAX_TMU];

    if (area == 0.0f)
        return;
    if (area < 0.0f) {
        Vertex swap = b;
        b = c;
        c = swap;
        area = -area;
    }
    for (unit = 0U; unit < MAX_TMU; ++unit)
        if (units & (1U << unit))
            slopes[unit] = texture_slope(&a, &b, &c, unit,
                -(c.sy - b.sy) / area, -(a.sy - c.sy) / area, -(b.sy - a.sy) / area,
                (c.sx - b.sx) / area, (a.sx - c.sx) / area, (b.sx - a.sx) / area);
    x0 = (int)floorf(fminf(a.sx, fminf(b.sx, c.sx)));
    x1 = (int)ceilf(fmaxf(a.sx, fmaxf(b.sx, c.sx)));
    y0 = (int)floorf(fminf(a.sy, fminf(b.sy, c.sy)));
    y1 = (int)ceilf(fmaxf(a.sy, fmaxf(b.sy, c.sy)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g_buffers.width) x1 = (int)g_buffers.width - 1;
    if (y1 >= g_buffers.height) y1 = (int)g_buffers.height - 1;
    if (x0 > x1 || y0 > y1) return;
    if (g_rix_collecting != 0) {
        if (g_rix_collecting > 0) {
            MiniGLRixVertex va = { a.sx, a.sy, a.sz, a.inv_w, a.color, a.u0, a.v0, a.q0 };
            MiniGLRixVertex vb = { b.sx, b.sy, b.sz, b.inv_w, b.color, b.u0, b.v0, b.q0 };
            MiniGLRixVertex vc = { c.sx, c.sy, c.sz, c.inv_w, c.color, c.u0, c.v0, c.q0 };
            if (!minigl_rix_backend_add_triangle(&g_rix, &va, &vb, &vc))
                g_rix_collecting = -1;
        }
        return;
    }
    mark_dirty(x0, y0, x1, y1);
    int is_fast_world = 0;
    int is_fast_lm_mt = 0;
    int is_fast_lm_blend = 0;
    int is_fast_alias = 0;

    const Texture *t0 = (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) ? g_bound_texture_info[0] : NULL;
    const Texture *t1 = (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) ? g_bound_texture_info[1] : NULL;
    const uint32_t *texels0 = NULL;
    const uint32_t *texels1 = NULL;
    float fw0 = 0.0f, fh0 = 0.0f;
    float fw1 = 0.0f, fh1 = 0.0f;
    uint32_t mask_x0 = 0U, mask_y0 = 0U;
    int max_x0 = 0, max_y0 = 0;
    int max_x1 = 0, max_y1 = 0;
    unsigned shift_y0 = 0U, shift_y1 = 0U;
    int depth_func_is_less = (g_state.depth_func == GL_LESS);
    int blend_is_mult = 0;
    int t0_internal_rgb = 0;
    int is_fast_hud = 0;
    uint8_t alpha_ref_byte = (uint8_t)(g_state.alpha_ref > 255U ? g_state.alpha_ref >> 8 : g_state.alpha_ref);
    int base_depth_ok = (g_depth_bits == 24U) &&
        (g_state.depth_func == GL_LEQUAL || g_state.depth_func == GL_LESS) &&
        (g_state.depth_near == 0U && g_state.depth_far == 0x00ffffffU) &&
        (g_state.depth_offset == 0.0f && polygon_offset == 0.0f) &&
        ((g_state.color_mask & 0xfU) == 0xfU) &&
        !(g_state.enabled_flags & (ZZ9K_MGL_STATE_ALPHA_TEST | ZZ9K_MGL_STATE_FOG | ZZ9K_MGL_STATE_SCISSOR_TEST));

    if (base_depth_ok &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE)) ==
            (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE) &&
        !(g_state.enabled_flags & ZZ9K_MGL_STATE_BLEND) &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1)) == ZZ9K_MGL_STATE_TEXTURE_2D_0 &&
        g_state.tex_env_mode[0] == GL_REPLACE &&
        t0 && t0->levels[0].size &&
        !mip_filter(t0->min_filter) && t0->min_filter == GL_LINEAR && t0->mag_filter == GL_LINEAR &&
        t0->wrap_s == GL_REPEAT && t0->wrap_t == GL_REPEAT &&
        (t0->levels[0].width & (t0->levels[0].width - 1U)) == 0U &&
        (t0->levels[0].height & (t0->levels[0].height - 1U)) == 0U)
    {
        is_fast_world = 1;
        t0_internal_rgb = t0->levels[0].internal_rgb;
        texels0 = (const uint32_t *)(g_buffers.texture_pool + t0->levels[0].offset);
        fw0 = (float)t0->levels[0].width;
        fh0 = (float)t0->levels[0].height;
        mask_x0 = t0->levels[0].width - 1U;
        mask_y0 = t0->levels[0].height - 1U;
        shift_y0 = (unsigned)__builtin_ctz((unsigned)t0->levels[0].width);
    }
    else if (base_depth_ok &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE)) ==
            (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE) &&
        !(g_state.enabled_flags & ZZ9K_MGL_STATE_BLEND) &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1)) ==
            (ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1) &&
        g_state.tex_env_mode[0] == GL_REPLACE &&
        g_state.tex_env_mode[1] == GL_MODULATE &&
        t0 && t0->levels[0].size &&
        !mip_filter(t0->min_filter) && t0->min_filter == GL_LINEAR && t0->mag_filter == GL_LINEAR &&
        t0->wrap_s == GL_REPEAT && t0->wrap_t == GL_REPEAT &&
        (t0->levels[0].width & (t0->levels[0].width - 1U)) == 0U &&
        (t0->levels[0].height & (t0->levels[0].height - 1U)) == 0U &&
        t1 && t1->levels[0].size &&
        !mip_filter(t1->min_filter) && t1->min_filter == GL_LINEAR && t1->mag_filter == GL_LINEAR &&
        t1->wrap_s == GL_CLAMP && t1->wrap_t == GL_CLAMP &&
        (t1->levels[0].width & (t1->levels[0].width - 1U)) == 0U &&
        (t1->levels[0].height & (t1->levels[0].height - 1U)) == 0U)
    {
        is_fast_lm_mt = 1;
        t0_internal_rgb = t0->levels[0].internal_rgb;
        texels0 = (const uint32_t *)(g_buffers.texture_pool + t0->levels[0].offset);
        fw0 = (float)t0->levels[0].width;
        fh0 = (float)t0->levels[0].height;
        mask_x0 = t0->levels[0].width - 1U;
        mask_y0 = t0->levels[0].height - 1U;
        shift_y0 = (unsigned)__builtin_ctz((unsigned)t0->levels[0].width);

        texels1 = (const uint32_t *)(g_buffers.texture_pool + t1->levels[0].offset);
        fw1 = (float)t1->levels[0].width;
        fh1 = (float)t1->levels[0].height;
        max_x1 = (int)t1->levels[0].width - 1;
        max_y1 = (int)t1->levels[0].height - 1;
        shift_y1 = (unsigned)__builtin_ctz((unsigned)t1->levels[0].width);
    }
    else if (base_depth_ok &&
        (g_state.enabled_flags & ZZ9K_MGL_STATE_DEPTH_TEST) &&
        !(g_state.enabled_flags & ZZ9K_MGL_STATE_DEPTH_WRITE) &&
        (g_state.enabled_flags & ZZ9K_MGL_STATE_BLEND) &&
        (g_state.blend_equation == ZZ9K_MGL_WIRE_FUNC_ADD || g_state.blend_equation == GL_FUNC_ADD) &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1)) == ZZ9K_MGL_STATE_TEXTURE_2D_0 &&
        (g_state.tex_env_mode[0] == GL_REPLACE || g_state.tex_env_mode[0] == GL_MODULATE) &&
        t0 && t0->levels[0].size &&
        !mip_filter(t0->min_filter) && t0->min_filter == GL_LINEAR && t0->mag_filter == GL_LINEAR &&
        t0->wrap_s == GL_CLAMP && t0->wrap_t == GL_CLAMP &&
        (t0->levels[0].width & (t0->levels[0].width - 1U)) == 0U &&
        (t0->levels[0].height & (t0->levels[0].height - 1U)) == 0U &&
        ((g_state.blend_src == GL_ZERO && g_state.blend_dst == GL_SRC_COLOR) ||
         (g_state.blend_src == GL_SRC_ALPHA && g_state.blend_dst == GL_ONE_MINUS_SRC_ALPHA)))
    {
        is_fast_lm_blend = 1;
        blend_is_mult = (g_state.blend_src == GL_ZERO && g_state.blend_dst == GL_SRC_COLOR);
        texels0 = (const uint32_t *)(g_buffers.texture_pool + t0->levels[0].offset);
        fw0 = (float)t0->levels[0].width;
        fh0 = (float)t0->levels[0].height;
        max_x0 = (int)t0->levels[0].width - 1;
        max_y0 = (int)t0->levels[0].height - 1;
        shift_y0 = (unsigned)__builtin_ctz((unsigned)t0->levels[0].width);
    }
    else if (base_depth_ok &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE)) ==
            (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE) &&
        !(g_state.enabled_flags & ZZ9K_MGL_STATE_BLEND) &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1)) == ZZ9K_MGL_STATE_TEXTURE_2D_0 &&
        g_state.tex_env_mode[0] == GL_MODULATE &&
        t0 && t0->levels[0].size &&
        !mip_filter(t0->min_filter) && t0->min_filter == GL_LINEAR && t0->mag_filter == GL_LINEAR &&
        t0->wrap_s == GL_REPEAT && t0->wrap_t == GL_REPEAT &&
        (t0->levels[0].width & (t0->levels[0].width - 1U)) == 0U &&
        (t0->levels[0].height & (t0->levels[0].height - 1U)) == 0U)
    {
        is_fast_alias = 1;
        t0_internal_rgb = t0->levels[0].internal_rgb;
        texels0 = (const uint32_t *)(g_buffers.texture_pool + t0->levels[0].offset);
        fw0 = (float)t0->levels[0].width;
        fh0 = (float)t0->levels[0].height;
        mask_x0 = t0->levels[0].width - 1U;
        mask_y0 = t0->levels[0].height - 1U;
        shift_y0 = (unsigned)__builtin_ctz((unsigned)t0->levels[0].width);
    }
    else if (!(g_state.enabled_flags & (ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_BLEND | ZZ9K_MGL_STATE_FOG | ZZ9K_MGL_STATE_SCISSOR_TEST)) &&
        (g_state.enabled_flags & (ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1)) == ZZ9K_MGL_STATE_TEXTURE_2D_0 &&
        (g_state.enabled_flags & ZZ9K_MGL_STATE_ALPHA_TEST) &&
        (g_state.alpha_func == GL_GREATER) &&
        (g_state.tex_env_mode[0] == GL_REPLACE) &&
        ((g_state.color_mask & 0xfU) == 0xfU) &&
        t0 && t0->levels[0].size &&
        t0->min_filter == GL_NEAREST && t0->mag_filter == GL_NEAREST &&
        (t0->levels[0].width & (t0->levels[0].width - 1U)) == 0U &&
        (t0->levels[0].height & (t0->levels[0].height - 1U)) == 0U &&
        t0->wrap_s == GL_CLAMP && t0->wrap_t == GL_CLAMP)
    {
        is_fast_hud = 1;
        texels0 = (const uint32_t *)(g_buffers.texture_pool + t0->levels[0].offset);
        fw0 = (float)t0->levels[0].width;
        fh0 = (float)t0->levels[0].height;
        max_x0 = (int)t0->levels[0].width - 1;
        max_y0 = (int)t0->levels[0].height - 1;
        shift_y0 = (unsigned)__builtin_ctz((unsigned)t0->levels[0].width);
    }

    if (is_fast_world || is_fast_lm_mt || is_fast_lm_blend || is_fast_alias || is_fast_hud) {
        RastPoint pa = rast_to_fixed(a.sx, a.sy);
        RastPoint pb = rast_to_fixed(b.sx, b.sy);
        RastPoint pc = rast_to_fixed(c.sx, c.sy);
        float C_bc_base = (c.sx - b.sx);
        float C_ca_base = (a.sx - c.sx);
        float C_ab_base = (b.sx - a.sx);
        float dy_bc = c.sy - b.sy;
        float dy_ca = a.sy - c.sy;
        float dy_ab = b.sy - a.sy;
        float B_bc = 0.5f - b.sx;
        float B_ca = 0.5f - c.sx;
        float B_ab = 0.5f - a.sx;

        for (y = y0; y <= y1; ++y) {
            float py = y + 0.5f;
            int left = x0, right = x1;
            edge_span(pb, pc, y, &left, &right);
            edge_span(pc, pa, y, &left, &right);
            edge_span(pa, pb, y, &left, &right);
            if (left > right) continue;

            float C_bc = C_bc_base * (py - b.sy);
            float C_ca = C_ca_base * (py - c.sy);
            float C_ab = C_ab_base * (py - a.sy);
            uint32_t *color_row = &g_buffers.color[(uint32_t)y * g_buffers.width];
            uint8_t *depth_row = g_buffers.depth + (size_t)y * g_buffers.width * 3U;

            for (x = left; x <= right; ++x) {
                float fx = (float)x;
                float edge_bc = C_bc - dy_bc * (fx + B_bc);
                float edge_ca = C_ca - dy_ca * (fx + B_ca);
                float edge_ab = C_ab - dy_ab * (fx + B_ab);
                float wa = edge_bc / area;
                float wb = edge_ca / area;
                float wc = edge_ab / area;

                float z = endpoint_depth ? a.sz : (wa * a.sz + wb * b.sz + wc * c.sz);
                if (z < 0.0f || z > 1.0f) continue;
                uint32_t depth_val = (uint32_t)(z * 16777215.0f + 0.5f);

                uint8_t *dp = depth_row + (size_t)x * 3U;
                uint32_t curr_depth = ((uint32_t)dp[0] << 16) | ((uint32_t)dp[1] << 8) | dp[2];
                if (depth_func_is_less ? (depth_val >= curr_depth) : (depth_val > curr_depth))
                    continue;

                float q0 = wa * a.q0 * a.inv_w + wb * b.q0 * b.inv_w + wc * c.q0 * c.inv_w;
                if (q0 == 0.0f) continue;
                float inv_q0 = 1.0f / q0;
                float u0 = (wa * a.u0 * a.inv_w + wb * b.u0 * b.inv_w + wc * c.u0 * c.inv_w) * inv_q0;
                float v0 = (wa * a.v0 * a.inv_w + wb * b.v0 * b.inv_w + wc * c.v0 * c.inv_w) * inv_q0;

                if (is_fast_world) {
                    dp[0] = (uint8_t)(depth_val >> 16);
                    dp[1] = (uint8_t)(depth_val >> 8);
                    dp[2] = (uint8_t)depth_val;
                    uint32_t texel = sample_pot_linear_repeat(texels0, u0, v0, fw0, fh0, mask_x0, mask_y0, shift_y0);
                    if (t0_internal_rgb) texel = (texel & 0xffffff00U) | (a.color & 0xffU);
                    color_row[x] = texel;
                } else if (is_fast_lm_mt) {
                    float q1 = wa * a.q1 * a.inv_w + wb * b.q1 * b.inv_w + wc * c.q1 * c.inv_w;
                    if (q1 == 0.0f) continue;
                    float inv_q1 = 1.0f / q1;
                    float u1 = (wa * a.u1 * a.inv_w + wb * b.u1 * b.inv_w + wc * c.u1 * c.inv_w) * inv_q1;
                    float v1 = (wa * a.v1 * a.inv_w + wb * b.v1 * b.inv_w + wc * c.v1 * c.inv_w) * inv_q1;

                    dp[0] = (uint8_t)(depth_val >> 16);
                    dp[1] = (uint8_t)(depth_val >> 8);
                    dp[2] = (uint8_t)depth_val;

                    uint32_t t0_col = sample_pot_linear_repeat(texels0, u0, v0, fw0, fh0, mask_x0, mask_y0, shift_y0);
                    if (t0_internal_rgb) t0_col = (t0_col & 0xffffff00U) | (a.color & 0xffU);
                    uint32_t t1_col = sample_pot_linear_clamp(texels1, u1, v1, fw1, fh1, max_x1, max_y1, shift_y1);
                    color_row[x] = modulate(t0_col, t1_col);
                } else if (is_fast_lm_blend) {
                    uint32_t lm_tex = sample_pot_linear_clamp(texels0, u0, v0, fw0, fh0, max_x0, max_y0, shift_y0);
                    uint32_t dst = color_row[x];
                    if (blend_is_mult) {
                        color_row[x] = modulate(dst, lm_tex);
                    } else {
                        color_row[x] = blend_alpha_fast(lm_tex, dst, (uint8_t)(lm_tex & 0xffU));
                    }
                } else if (is_fast_hud) {
                    float q0 = wa * a.q0 * a.inv_w + wb * b.q0 * b.inv_w + wc * c.q0 * c.inv_w;
                    if (q0 == 0.0f) continue;
                    float inv_q0 = 1.0f / q0;
                    float u0 = (wa * a.u0 * a.inv_w + wb * b.u0 * b.inv_w + wc * c.u0 * c.inv_w) * inv_q0;
                    float v0 = (wa * a.v0 * a.inv_w + wb * b.v0 * b.inv_w + wc * c.v0 * c.inv_w) * inv_q0;
                    uint32_t texel = sample_pot_nearest_clamp(texels0, u0, v0, fw0, fh0, max_x0, max_y0, shift_y0);
                    if ((uint8_t)texel <= alpha_ref_byte) continue;
                    color_row[x] = texel;
                } else if (is_fast_alias) {
                    dp[0] = (uint8_t)(depth_val >> 16);
                    dp[1] = (uint8_t)(depth_val >> 8);
                    dp[2] = (uint8_t)depth_val;

                    float denominator = wa * a.inv_w + wb * b.inv_w + wc * c.inv_w;
                    uint32_t prim_col = (g_state.shade_model == GL_FLAT) ? a.flat_color :
                                        interp_color(&a, &b, &c, wa, wb, wc, denominator);
                    uint32_t t0_col = sample_pot_linear_repeat(texels0, u0, v0, fw0, fh0, mask_x0, mask_y0, shift_y0);
                    if (t0_internal_rgb) t0_col = (t0_col & 0xffffff00U) | (prim_col & 0xffU);
                    color_row[x] = modulate(prim_col, t0_col);
                }
            }
        }
        return;
    }
    RastPoint pa = rast_to_fixed(a.sx, a.sy);
    RastPoint pb = rast_to_fixed(b.sx, b.sy);
    RastPoint pc = rast_to_fixed(c.sx, c.sy);
    for (y = y0; y <= y1; ++y) {
        float py = y + 0.5f;
        float wa;
        float wb;
        float wc;
        int left = x0;
        int right = x1;
        edge_span(pb, pc, y, &left, &right);
        edge_span(pc, pa, y, &left, &right);
        edge_span(pa, pb, y, &left, &right);
        for (x = left; x <= right; ++x) {
            wa = edge(&b, &c, x + 0.5f, py) / area;
            wb = edge(&c, &a, x + 0.5f, py) / area;
            wc = edge(&a, &b, x + 0.5f, py) / area;
            if (wa < 0.0f) wa = 0.0f;
            if (wb < 0.0f) wb = 0.0f;
            if (wc < 0.0f) wc = 0.0f;
            float denominator;
            float z;
            float u0;
            float v0;
            float u1;
            float v1;
            float q0;
            float q1;
            float fog_coord;

            TextureGradient pixel_gradients[MAX_TMU];
            denominator = wa * a.inv_w + wb * b.inv_w + wc * c.inv_w;
            if (denominator == 0.0f)
                continue;
            z = endpoint_depth ? a.sz : wa * a.sz + wb * b.sz + wc * c.sz;
            if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) {
                q0 = wa * a.q0 * a.inv_w + wb * b.q0 * b.inv_w + wc * c.q0 * c.inv_w;
                if (q0 == 0.0f) continue;
                u0 = (wa * a.u0 * a.inv_w + wb * b.u0 * b.inv_w + wc * c.u0 * c.inv_w) / q0;
                v0 = (wa * a.v0 * a.inv_w + wb * b.v0 * b.inv_w + wc * c.v0 * c.inv_w) / q0;
                if (units & 1U) pixel_gradients[0] = texture_gradient(&slopes[0], u0, v0, q0);
            } else { u0 = 0.0f; v0 = 0.0f; }
            if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) {
                q1 = wa * a.q1 * a.inv_w + wb * b.q1 * b.inv_w + wc * c.q1 * c.inv_w;
                if (q1 == 0.0f) continue;
                u1 = (wa * a.u1 * a.inv_w + wb * b.u1 * b.inv_w + wc * c.u1 * c.inv_w) / q1;
                v1 = (wa * a.v1 * a.inv_w + wb * b.v1 * b.inv_w + wc * c.v1 * c.inv_w) / q1;
                if (units & 2U) pixel_gradients[1] = texture_gradient(&slopes[1], u1, v1, q1);
            } else { u1 = 0.0f; v1 = 0.0f; }

            fog_coord = (g_state.enabled_flags & ZZ9K_MGL_STATE_FOG) ?
                (wa * a.fog_coord * a.inv_w + wb * b.fog_coord * b.inv_w + wc * c.fog_coord * c.inv_w) / denominator : 0.0f;
            fragment(x, y, z, u0, v0, u1, v1, fog_coord,
                     interp_color(&a, &b, &c, wa, wb, wc, denominator), units ? pixel_gradients : NULL, polygon_offset);
        }
    }
}

static void line_screen(const Vertex *a, const Vertex *b, float polygon_offset)
{
    float dx = b->sx - a->sx;
    float dy = b->sy - a->sy;
    int width = g_state.line_width ? g_state.line_width : 1;
    unsigned units = gradient_units();
    unsigned unit;
    TextureSlope slopes[MAX_TMU];

    if (dx == 0.0f && dy == 0.0f) {
        int cx = (int)floorf(a->sx + 0.5f);
        int cy = (int)floorf(a->sy + 0.5f);
        int x_start = cx - width / 2;
        int y_start = cy - width / 2;
        int x, y;
        for (y = 0; y < width; ++y)
            for (x = 0; x < width; ++x)
                fragment(x_start + x, y_start + y, a->sz, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, a->color, NULL, polygon_offset);
        return;
    }

    for (unit = 0U; unit < MAX_TMU; ++unit)
        if (units & (1U << unit)) {
            float squared = dx * dx + dy * dy;
            float ax = dx / squared;
            float ay = dy / squared;
            slopes[unit] = texture_slope(a, b, b, unit, -ax, ax, 0.0f, -ay, ay, 0.0f);
        }

    mark_dirty((int)floorf(fminf(a->sx, b->sx)) - width / 2, (int)floorf(fminf(a->sy, b->sy)) - width / 2,
               (int)ceilf(fmaxf(a->sx, b->sx)) + width / 2, (int)ceilf(fmaxf(a->sy, b->sy)) + width / 2);

    if (fabsf(dx) >= fabsf(dy)) {
        /* X-major line: step along X, widen column along Y */
        float x_min = fminf(a->sx, b->sx);
        float x_max = fmaxf(a->sx, b->sx);
        int start_x = (int)ceilf(x_min - 0.5f);
        int end_x = (int)floorf(x_max - 0.5f);

        for (int x = start_x; x <= end_x; ++x) {
            float px = (float)x + 0.5f;
            float amount = (px - a->sx) / dx;
            float py = a->sy + dy * amount;
            float denominator = (1.0f - amount) * a->inv_w + amount * b->inv_w;
            if (denominator == 0.0f) continue;
            float z = a->sz + (b->sz - a->sz) * amount;

            float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
            TextureGradient pixel_gradients[MAX_TMU];
            if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) {
                float q0 = (1.0f - amount) * a->q0 * a->inv_w + amount * b->q0 * b->inv_w;
                if (q0 == 0.0f) continue;
                u0 = ((1.0f - amount) * a->u0 * a->inv_w + amount * b->u0 * b->inv_w) / q0;
                v0 = ((1.0f - amount) * a->v0 * a->inv_w + amount * b->v0 * b->inv_w) / q0;
                if (units & 1U) pixel_gradients[0] = texture_gradient(&slopes[0], u0, v0, q0);
            }
            if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) {
                float q1 = (1.0f - amount) * a->q1 * a->inv_w + amount * b->q1 * b->inv_w;
                if (q1 == 0.0f) continue;
                u1 = ((1.0f - amount) * a->u1 * a->inv_w + amount * b->u1 * b->inv_w) / q1;
                v1 = ((1.0f - amount) * a->v1 * a->inv_w + amount * b->v1 * b->inv_w) / q1;
                if (units & 2U) pixel_gradients[1] = texture_gradient(&slopes[1], u1, v1, q1);
            }
            float fog_coord = ((1.0f - amount) * a->fog_coord * a->inv_w + amount * b->fog_coord * b->inv_w) / denominator;
            uint32_t color = g_state.shade_model == GL_FLAT ? a->flat_color : color_lerp(a->color, b->color, amount);

            /* Minor-axis span: pixel centres inside the half-open interval
             * [c - w/2, c + w/2), as llvmpipe covers wide aliased lines
             * (a width-1 line on an integer centre covers the pixel below). */
            int y_first = (int)ceilf(py - (float)width * 0.5f - 0.5f);
            int y_last = (int)ceilf(py + (float)width * 0.5f - 0.5f) - 1;
            for (int w = y_first; w <= y_last; ++w) {
                fragment(x, w, z, u0, v0, u1, v1, fog_coord, color, units ? pixel_gradients : NULL, polygon_offset);
            }
        }
    } else {
        /* Y-major line: step along Y, widen row along X */
        float y_min = fminf(a->sy, b->sy);
        float y_max = fmaxf(a->sy, b->sy);
        int start_y = (int)ceilf(y_min - 0.5f);
        int end_y = (int)floorf(y_max - 0.5f);

        for (int y = start_y; y <= end_y; ++y) {
            float py = (float)y + 0.5f;
            float amount = (py - a->sy) / dy;
            float px = a->sx + dx * amount;
            float denominator = (1.0f - amount) * a->inv_w + amount * b->inv_w;
            if (denominator == 0.0f) continue;
            float z = a->sz + (b->sz - a->sz) * amount;

            float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
            TextureGradient pixel_gradients[MAX_TMU];
            if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) {
                float q0 = (1.0f - amount) * a->q0 * a->inv_w + amount * b->q0 * b->inv_w;
                if (q0 == 0.0f) continue;
                u0 = ((1.0f - amount) * a->u0 * a->inv_w + amount * b->u0 * b->inv_w) / q0;
                v0 = ((1.0f - amount) * a->v0 * a->inv_w + amount * b->v0 * b->inv_w) / q0;
                if (units & 1U) pixel_gradients[0] = texture_gradient(&slopes[0], u0, v0, q0);
            }
            if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) {
                float q1 = (1.0f - amount) * a->q1 * a->inv_w + amount * b->q1 * b->inv_w;
                if (q1 == 0.0f) continue;
                u1 = ((1.0f - amount) * a->u1 * a->inv_w + amount * b->u1 * b->inv_w) / q1;
                v1 = ((1.0f - amount) * a->v1 * a->inv_w + amount * b->v1 * b->inv_w) / q1;
                if (units & 2U) pixel_gradients[1] = texture_gradient(&slopes[1], u1, v1, q1);
            }
            float fog_coord = ((1.0f - amount) * a->fog_coord * a->inv_w + amount * b->fog_coord * b->inv_w) / denominator;
            uint32_t color = g_state.shade_model == GL_FLAT ? a->flat_color : color_lerp(a->color, b->color, amount);

            /* Minor-axis span: pixel centres inside the half-open interval
             * [c - w/2, c + w/2), as llvmpipe covers wide aliased lines. */
            int x_first = (int)ceilf(px - (float)width * 0.5f - 0.5f);
            int x_last = (int)ceilf(px + (float)width * 0.5f - 0.5f) - 1;
            for (int w = x_first; w <= x_last; ++w) {
                fragment(w, y, z, u0, v0, u1, v1, fog_coord, color, units ? pixel_gradients : NULL, polygon_offset);
            }
        }
    }
}

static void point_screen(const Vertex *vertex, float polygon_offset)
{
    int size = g_state.point_size ? g_state.point_size : 1;
    /* Pixels whose centres lie in the half-open square [c - s/2, c + s/2),
     * the same llvmpipe convention line_screen applies to its minor axis:
     * a size-1 point at a half-integer centre covers the pixel containing
     * that centre, and a boundary centre on the low edge is included while
     * one on the high edge is not. */
    int x_first = (int)ceilf(vertex->sx - (float)size * 0.5f - 0.5f);
    int x_last = (int)ceilf(vertex->sx + (float)size * 0.5f - 0.5f) - 1;
    int y_first = (int)ceilf(vertex->sy - (float)size * 0.5f - 0.5f);
    int y_last = (int)ceilf(vertex->sy + (float)size * 0.5f - 0.5f) - 1;
    int x, y;

    mark_dirty(x_first, y_first, x_last, y_last);
    for (y = y_first; y <= y_last; ++y)
        for (x = x_first; x <= x_last; ++x)
            fragment(x, y, vertex->sz,
                     vertex->q0 != 0.0f ? vertex->u0 / vertex->q0 : 0.0f,
                     vertex->q0 != 0.0f ? vertex->v0 / vertex->q0 : 0.0f,
                     vertex->q1 != 0.0f ? vertex->u1 / vertex->q1 : 0.0f,
                     vertex->q1 != 0.0f ? vertex->v1 / vertex->q1 : 0.0f,
                     vertex->fog_coord, vertex->flat_color, NULL, polygon_offset);
}

static void render_clipped_polygon(const Vertex *clipped, unsigned count, uint32_t flat_color)
{
    float area;
    float polygon_offset;
    int front;
    uint16_t face;
    Vertex verts[CLIP_MAX_VERTICES];
    unsigned i;

    if (count < 3U)
        return;
    area = 0.0f;
    for (i = 2U; i < count; ++i)
        area += edge(&clipped[0], &clipped[i - 1U], clipped[i].sx, clipped[i].sy);
    if (g_state.min_triangle_area > 0.0f && fabsf(area) < 2.0f * g_state.min_triangle_area)
        return;
    front = (g_state.front_face ? g_state.front_face : GL_CCW) == GL_CCW ? area < 0.0f : area > 0.0f;
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_CULL_FACE) {
        face = g_state.cull_face_mode ? g_state.cull_face_mode : GL_BACK;
        if (face == GL_FRONT_AND_BACK || (front && face == GL_FRONT) || (!front && face == GL_BACK))
            return;
    }
    for (i = 0U; i < count; ++i) {
        verts[i] = clipped[i];
        verts[i].color = front ? verts[i].front_color : verts[i].back_color;
        verts[i].flat_color = flat_color;
    }
    polygon_offset = polygon_offset_polygon(verts, count);
    switch (g_state.polygon_mode ? g_state.polygon_mode : GL_FILL) {
    case GL_POINT:
        for (i = 0U; i < count; ++i)
            point_screen(&verts[i], polygon_offset);
        break;
    case GL_LINE:
        for (i = 0U; i < count; ++i)
            line_screen(&verts[i], &verts[(i + 1U) % count], polygon_offset);
        break;
    default:
        for (i = 2U; i < count; ++i)
            triangle_fill(&verts[0], &verts[i - 1U], &verts[i], polygon_offset);
        break;
    }
}

static void primitive_triangle(const Vertex *first, const Vertex *second, const Vertex *third)
{
    Vertex input[3];
    Vertex clipped[CLIP_MAX_VERTICES];
    unsigned count;
    unsigned index;

    input[0] = *first;
    input[1] = *second;
    input[2] = *third;
    input[0].flat_color = input[1].flat_color = input[2].flat_color = third->color;
    count = clip_polygon(input, 3U, clipped);
    if (!count)
        return;
    for (index = 0U; index < count; ++index)
        if (!project(&clipped[index]))
            return;
    render_clipped_polygon(clipped, count, third->color);
}

static void primitive_quad(const Vertex *first, const Vertex *second, const Vertex *third, const Vertex *fourth)
{
    Vertex input[4];
    Vertex clipped[CLIP_MAX_VERTICES];
    unsigned count;
    unsigned index;

    input[0] = *first;
    input[1] = *second;
    input[2] = *third;
    input[3] = *fourth;
    input[0].flat_color = input[1].flat_color = input[2].flat_color = input[3].flat_color = fourth->color;
    count = clip_polygon(input, 4U, clipped);
    if (!count)
        return;
    for (index = 0U; index < count; ++index)
        if (!project(&clipped[index]))
            return;
    render_clipped_polygon(clipped, count, fourth->color);
}

static void primitive_polygon(const Vertex *in, unsigned in_count)
{
    Vertex input[CLIP_MAX_VERTICES];
    Vertex clipped[CLIP_MAX_VERTICES];
    unsigned count;
    unsigned index;

    if (in_count < 3U || in_count + 6U > CLIP_MAX_VERTICES) {
        g_error |= RASTER_ERR_MEMORY;
        return;
    }
    for (index = 0U; index < in_count; ++index) {
        input[index] = in[index];
        input[index].flat_color = in[0].color;
    }
    count = clip_polygon(input, in_count, clipped);
    if (!count)
        return;
    for (index = 0U; index < count; ++index)
        if (!project(&clipped[index]))
            return;
    render_clipped_polygon(clipped, count, in[0].color);
}

static void primitive_line(const Vertex *first, const Vertex *second)
{
    Vertex input[2];
    Vertex clipped[CLIP_MAX_VERTICES];
    unsigned count;

    input[0] = *first;
    input[1] = *second;
    input[0].flat_color = input[1].flat_color = second->color;
    count = clip_polygon(input, 2U, clipped);
    if (count < 2U || !project(&clipped[0]) || !project(&clipped[count - 1U]))
        return;
    line_screen(&clipped[0], &clipped[count - 1U], 0.0f);
}

static void primitive_point(const Vertex *vertex)
{
    Vertex projected = *vertex;
    unsigned index;

    for (index = 0U; index < 6U; ++index)
        if (plane(&projected, index) < 0.0f)
            return;
    if (project(&projected))
        point_screen(&projected, 0.0f);
}

static uint32_t index_at(const uint8_t *indices, uint32_t first, uint32_t index)
{
    return indices ? be16(indices + (size_t)index * 2U) : first + index;
}

static int fetch_indexed(uint16_t stride, uint32_t base, const uint8_t *indices,
                         uint32_t first, uint32_t index, Vertex *out)
{
    uint32_t key = index_at(indices, first, index);
    unsigned slot = key & (VERTEX_CACHE_SIZE - 1U);
    if (g_vertex_cache_epoch[slot] == g_draw_epoch && g_vertex_cache_index[slot] == key) {
        *out = g_vertex_cache[slot];
        return 1;
    }
    if (!fetch(stride, base, key, out)) return 0;
    g_vertex_cache[slot] = *out;
    g_vertex_cache_index[slot] = key;
    g_vertex_cache_epoch[slot] = g_draw_epoch;
    return 1;
}

typedef struct PolygonStream {
    Vertex first[6];
    Vertex last[6];
    uint8_t seen[6];
    Vertex anchor;
    Vertex previous;
    unsigned count;
    uint32_t flat_color;
    float area;
    float depth_bias;
    int depth_bias_ready;
    int front;
    int rejected;
    int measure;
} PolygonStream;

static void polygon_stream_emit(PolygonStream *stream, unsigned stage, const Vertex *input)
{
    Vertex vertex = *input;
    if (stage < 6U) {
        if (!stream->seen[stage]) {
            stream->first[stage] = vertex;
            stream->last[stage] = vertex;
            stream->seen[stage] = 1U;
            return;
        }
        {
            Vertex *last = &stream->last[stage];
            float da = plane(last, stage);
            float db = plane(&vertex, stage);
            if (da >= 0.0f)
                polygon_stream_emit(stream, stage + 1U, last);
            if ((da >= 0.0f) != (db >= 0.0f)) {
                Vertex crossing = lerp_vertex(last, &vertex, da / (da - db));
                polygon_stream_emit(stream, stage + 1U, &crossing);
            }
            *last = vertex;
        }
        return;
    }
    if (!project(&vertex)) { stream->rejected = 1; return; }
    vertex.flat_color = stream->flat_color;
    if (stream->count == 0U) stream->anchor = vertex;
    else if (stream->count >= 2U) {
        float area = edge(&stream->anchor, &stream->previous, vertex.sx, vertex.sy);
        stream->area += area;
        if (!stream->depth_bias_ready &&
            (g_state.polygon_offset_factor == 0.0f ||
             !(g_state.enabled_flags & polygon_offset_flag(g_state.polygon_mode ?
                                                      g_state.polygon_mode : GL_FILL)) ||
             area != 0.0f)) {
            stream->depth_bias = polygon_offset_bias(&stream->anchor, &stream->previous, &vertex);
            stream->depth_bias_ready = 1;
        }
        if (stream->count == 2U) {

            stream->anchor.color = stream->front ? stream->anchor.front_color : stream->anchor.back_color;
            stream->previous.color = stream->front ? stream->previous.front_color : stream->previous.back_color;
            if (!stream->measure && !stream->rejected) {
                if (g_state.polygon_mode == GL_POINT) { point_screen(&stream->anchor, stream->depth_bias); point_screen(&stream->previous, stream->depth_bias); }
                else if (g_state.polygon_mode == GL_LINE) line_screen(&stream->anchor, &stream->previous, stream->depth_bias);
            }
        }
        vertex.color = stream->front ? vertex.front_color : vertex.back_color;
        if (!stream->measure && !stream->rejected) {
            if (g_state.polygon_mode == GL_POINT) point_screen(&vertex, stream->depth_bias);
            else if (g_state.polygon_mode == GL_LINE) line_screen(&stream->previous, &vertex, stream->depth_bias);
            else triangle_fill(&stream->anchor, &stream->previous, &vertex, stream->depth_bias);
        }
    }
    stream->previous = vertex;
    ++stream->count;
}

static void polygon_stream_finish(PolygonStream *stream)
{
    unsigned stage;
    for (stage = 0U; stage < 6U; ++stage)
        if (stream->seen[stage]) {
            Vertex first = stream->first[stage];
            polygon_stream_emit(stream, stage, &first);
        }
    if (!stream->measure && !stream->rejected && stream->count >= 3U && g_state.polygon_mode == GL_LINE)
        line_screen(&stream->previous, &stream->anchor, stream->depth_bias);
}

static void large_polygon(uint16_t stride, uint32_t base, const uint8_t *indices,
                          uint32_t first, uint32_t count)
{
    PolygonStream stream;
    uint32_t index;
    unsigned pass;
    float depth_bias = 0.0f;
    int depth_bias_ready = 0;
    int front = 0;
    for (pass = 0U; pass < 2U; ++pass) {
        memset(&stream, 0, sizeof(stream));
        stream.measure = pass == 0U;
        stream.front = front;
        stream.depth_bias = depth_bias;
        stream.depth_bias_ready = depth_bias_ready;
        for (index = 0U; index < count; ++index) {
            Vertex vertex;
            if (!fetch_indexed(stride, base, indices, first, index, &vertex)) return;
            if (index == 0U) stream.flat_color = vertex.color;
            polygon_stream_emit(&stream, 0U, &vertex);
        }
        polygon_stream_finish(&stream);
        if (pass == 0U && !stream.depth_bias_ready && stream.count >= 2U) {
            stream.depth_bias = polygon_offset_bias(&stream.anchor, &stream.previous, &stream.previous);
            stream.depth_bias_ready = 1;
        }
        if (pass == 0U) {
            uint16_t face = g_state.cull_face_mode ? g_state.cull_face_mode : GL_BACK;
            depth_bias = stream.depth_bias;
            depth_bias_ready = stream.depth_bias_ready;
            if (stream.rejected || stream.count < 3U ||
                fabsf(stream.area) < 2.0f * g_state.min_triangle_area) return;
            front = (g_state.front_face ? g_state.front_face : GL_CCW) == GL_CCW ? stream.area < 0.0f : stream.area > 0.0f;
            if ((g_state.enabled_flags & ZZ9K_MGL_STATE_CULL_FACE) &&
                (face == GL_FRONT_AND_BACK || (front && face == GL_FRONT) || (!front && face == GL_BACK))) return;
        }
    }
}

static void assemble(uint16_t mode, uint16_t stride, uint32_t base, const uint8_t *indices,
                     uint32_t first, uint32_t count)
{
    uint32_t index;
    Vertex a;
    Vertex b;
    Vertex c;
    Vertex d;
    if (++g_draw_epoch == 0U) {
        memset(g_vertex_cache_epoch, 0, sizeof(g_vertex_cache_epoch));
        g_draw_epoch = 1U;
    }
    if (mode == GL_POINTS) {
        for (index = 0U; index < count; ++index)
            if (fetch_indexed(stride, base, indices, first, index, &a)) primitive_point(&a);
        return;
    }
    if (mode == GL_LINES) {
        for (index = 1U; index < count; index += 2U)
            if (fetch_indexed(stride, base, indices, first, index - 1U, &a) &&
                fetch_indexed(stride, base, indices, first, index, &b)) primitive_line(&a, &b);
        return;
    }
    if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
        for (index = 1U; index < count; ++index)
            if (fetch_indexed(stride, base, indices, first, index - 1U, &a) &&
                fetch_indexed(stride, base, indices, first, index, &b)) primitive_line(&a, &b);
        if (mode == GL_LINE_LOOP && count > 2U &&
            fetch_indexed(stride, base, indices, first, count - 1U, &a) &&
            fetch_indexed(stride, base, indices, first, 0U, &b)) primitive_line(&a, &b);
        return;
    }
    if (mode == GL_TRIANGLES) {
        for (index = 2U; index < count; index += 3U)
            if (fetch_indexed(stride, base, indices, first, index - 2U, &a) &&
                fetch_indexed(stride, base, indices, first, index - 1U, &b) &&
                fetch_indexed(stride, base, indices, first, index, &c)) primitive_triangle(&a, &b, &c);
        return;
    }
    if (mode == GL_TRIANGLE_STRIP) {
        for (index = 2U; index < count; ++index) {
            if (fetch_indexed(stride, base, indices, first, index - 2U, &a) &&
                fetch_indexed(stride, base, indices, first, index - 1U, &b) &&
                fetch_indexed(stride, base, indices, first, index, &c)) {
                if (index & 1U) primitive_triangle(&b, &a, &c);
                else primitive_triangle(&a, &b, &c);
            }
        }
        return;
    }
    if (mode == GL_TRIANGLE_FAN) {
        for (index = 2U; index < count; ++index)
            if (fetch_indexed(stride, base, indices, first, 0U, &a) &&
                fetch_indexed(stride, base, indices, first, index - 1U, &b) &&
                fetch_indexed(stride, base, indices, first, index, &c)) primitive_triangle(&a, &b, &c);
        return;
    }
    if (mode == GL_POLYGON) {
        Vertex poly[CLIP_MAX_VERTICES];
        if (count > CLIP_MAX_VERTICES - 6U) {
            large_polygon(stride, base, indices, first, count);
            return;
        }
        if (count < 3U) {
            g_error |= RASTER_ERR_MEMORY;
            return;
        }
        for (index = 0U; index < count; ++index)
            if (!fetch_indexed(stride, base, indices, first, index, &poly[index])) return;
        primitive_polygon(poly, count);
        return;
    }
    if (mode == GL_QUADS) {
        for (index = 3U; index < count; index += 4U)
            if (fetch_indexed(stride, base, indices, first, index - 3U, &a) &&
                fetch_indexed(stride, base, indices, first, index - 2U, &b) &&
                fetch_indexed(stride, base, indices, first, index - 1U, &c) &&
                fetch_indexed(stride, base, indices, first, index, &d)) {
                primitive_quad(&a, &b, &c, &d);
            }
        return;
    }
    if (mode == GL_QUAD_STRIP) {
        for (index = 3U; index < count; index += 2U)
            if (fetch_indexed(stride, base, indices, first, index - 3U, &a) &&
                fetch_indexed(stride, base, indices, first, index - 2U, &b) &&
                fetch_indexed(stride, base, indices, first, index - 1U, &c) &&
                fetch_indexed(stride, base, indices, first, index, &d)) {
                primitive_quad(&a, &b, &d, &c);
            }
        return;
    }
    g_error |= RASTER_ERR_VERTEX;
}

void minigl_raster_init(void)
{
    unsigned index;

    /* Wire-v9 contexts supply all renderer storage before initialization.
     * Never resurrect the retired MGL5 fixed DDR staging window here. */
    if (!ready()) {
        g_error |= RASTER_ERR_MEMORY;
        return;
    }
    minigl_rix_backend_init(&g_rix, g_rix_platform_set ? &g_rix_platform : NULL);
    for (index = 0U; index < 4U; ++index)
        identity(g_matrix[index]);
    update_mvp();
    memset(g_stack_depth, 0, sizeof(g_stack_depth));
    memset(&g_state, 0, sizeof(g_state));
    memset(g_textures, 0, sizeof(g_textures));
    texture_initialize(&g_default_texture, 0U);
    g_texture_allocations = NULL;
    g_texture_used = 0U;
    memset(g_texture_lookup, 0, sizeof(g_texture_lookup));
    memset(g_bound_texture, 0, sizeof(g_bound_texture));
    for (index = 0U; index < MAX_TMU; ++index) g_bound_texture_info[index] = &g_default_texture;
    g_dirty_valid = 0;
    g_depth_bits = 24U;
    g_state.blend_src = GL_ONE;
    g_state.blend_dst = GL_ZERO;
    g_state.blend_src_alpha = GL_ONE;
    g_state.blend_dst_alpha = GL_ZERO;
    g_state.blend_equation = GL_FUNC_ADD;
    g_state.depth_func = GL_LESS;
    g_state.depth_far = 0x00ffffffU;
    g_state.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    g_polygon_offset_units = g_state.polygon_offset_units / (float)depth_max();
    g_polygon_depth_scale = ((float)g_state.depth_far - (float)g_state.depth_near) / 16777215.0f;
    g_state.color_mask = 0xfU;
    g_state.viewport_w = g_buffers.width;
    g_state.viewport_h = g_buffers.height;
    g_state.polygon_mode = GL_FILL;
    g_state.point_size = 1U;
    g_state.line_width = 1U;
    g_state.tex_env_mode[0] = GL_MODULATE;
    g_state.tex_env_mode[1] = GL_MODULATE;
    minigl_lighting_reset();
    minigl_lighting_modelview(g_matrix[0]);
}

void minigl_raster_clear(uint32_t mask, uint32_t color, uint32_t depth)
{
    int x0 = 0;
    int y0 = 0;
    int x1;
    int y1;
    int32_t y;
    int32_t x;
    uint32_t scaled_depth;

    if (!ready())
        return;
    if (!g_context_backend_logged) {
        g_context_backend_logged = 1;
        minigl_rix_backend_log_context_backend(&g_rix);
    }
    if (minigl_rix_backend_available(&g_rix) &&
        minigl_rix_backend_prepare_target(&g_rix, g_buffers.color, pixels()) &&
        minigl_rix_backend_clear(&g_rix, g_buffers.width, g_buffers.height,
                                 (uintptr_t)g_buffers.color, (uintptr_t)g_buffers.depth,
                                 mask, color, depth)) {
        if (((mask & GL_COLOR_BUFFER_BIT) != 0U) || ((mask & 1U) != 0U))
            mark_dirty(0, 0, (int)g_buffers.width - 1, (int)g_buffers.height - 1);
        return;
    }
    if (!rix_cpu_visible())
        return;
    scaled_depth = depth >> (24U - (g_depth_bits == 16U ? 16U : 24U));
    x1 = (int)g_buffers.width - 1;
    y1 = (int)g_buffers.height - 1;
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_SCISSOR_TEST) {
        /* The same rectangle the per-pixel scissor test accepted: clamp the
         * clear loops instead of testing every pixel. */
        int32_t left = g_state.scissor_x;
        int32_t right = left + g_state.scissor_w;
        int32_t top = (int32_t)g_buffers.height - g_state.scissor_y - g_state.scissor_h;
        if (left > x0) x0 = (int)left;
        if (right - 1 < x1) x1 = (int)right - 1;
        if (top > y0) y0 = (int)top;
        if (top + (int32_t)g_state.scissor_h - 1 < y1)
            y1 = (int)(top + (int32_t)g_state.scissor_h - 1);
        if (x0 > x1 || y0 > y1)
            return;
    }
    {
        uint32_t cmask = g_state.color_mask & 0xfU;
        int write_color = (((mask & GL_COLOR_BUFFER_BIT) != 0U) || ((mask & 1U) != 0U)) && cmask != 0U;
        int write_depth = (((mask & GL_DEPTH_BUFFER_BIT) != 0U) || ((mask & 2U) != 0U));
        if (!write_color && !write_depth)
            return;
        if (write_color)
            mark_dirty(x0, y0, x1, y1);
        uint32_t total_pix = (uint32_t)g_buffers.width * g_buffers.height;
        if (x0 == 0 && y0 == 0 && x1 == (int)g_buffers.width - 1 && y1 == (int)g_buffers.height - 1) {
            if (write_color) {
                if (cmask == 0xfU) {
                    for (uint32_t i = 0; i < total_pix; ++i) g_buffers.color[i] = color;
                } else {
                    for (uint32_t i = 0; i < total_pix; ++i) g_buffers.color[i] = mask_color(g_buffers.color[i], color);
                }
            }
            if (write_depth) {
                if (g_depth_bits == 16U) {
                    uint8_t d0 = (uint8_t)(scaled_depth >> 8);
                    uint8_t d1 = (uint8_t)scaled_depth;
                    if (d0 == d1) {
                        memset(g_buffers.depth, d0, total_pix * 2U);
                    } else {
                        uint8_t *dp = g_buffers.depth;
                        for (uint32_t i = 0; i < total_pix; ++i) {
                            *dp++ = d0; *dp++ = d1;
                        }
                    }
                } else {
                    if (scaled_depth == 0x00ffffffU) {
                        memset(g_buffers.depth, 0xff, total_pix * 3U);
                    } else if (scaled_depth == 0U) {
                        memset(g_buffers.depth, 0x00, total_pix * 3U);
                    } else {
                        uint8_t d0 = (uint8_t)(scaled_depth >> 16);
                        uint8_t d1 = (uint8_t)(scaled_depth >> 8);
                        uint8_t d2 = (uint8_t)scaled_depth;
                        uint8_t *dp = g_buffers.depth;
                        for (uint32_t i = 0; i < total_pix; ++i) {
                            *dp++ = d0; *dp++ = d1; *dp++ = d2;
                        }
                    }
                }
            }
            return;
        }

        if (g_depth_bits == 16U) {
            for (y = y0; y <= y1; ++y) {
                uint32_t *row = &g_buffers.color[(uint32_t)y * g_buffers.width];
                uint8_t *drow = g_buffers.depth + (size_t)y * g_buffers.width * 2U;
                for (x = x0; x <= x1; ++x) {
                    if (write_color)
                        row[x] = cmask == 0xfU ? color : mask_color(row[x], color);
                    if (write_depth) {
                        drow[(size_t)x * 2U] = (uint8_t)(scaled_depth >> 8);
                        drow[(size_t)x * 2U + 1U] = (uint8_t)scaled_depth;
                    }
                }
            }
        } else {
            for (y = y0; y <= y1; ++y) {
                uint32_t *row = &g_buffers.color[(uint32_t)y * g_buffers.width];
                uint8_t *drow = g_buffers.depth + (size_t)y * g_buffers.width * 3U;
                for (x = x0; x <= x1; ++x) {
                    if (write_color)
                        row[x] = cmask == 0xfU ? color : mask_color(row[x], color);
                    if (write_depth) {
                        drow[(size_t)x * 3U] = (uint8_t)(scaled_depth >> 16);
                        drow[(size_t)x * 3U + 1U] = (uint8_t)(scaled_depth >> 8);
                        drow[(size_t)x * 3U + 2U] = (uint8_t)scaled_depth;
                    }
                }
            }
        }
    }
}

void minigl_raster_set_state(const ZZ9KMiniGLPacketSetState *state)
{
    if (!state)
        return;
    memcpy(&g_state, state, sizeof(g_state));
    minigl_rix_backend_set_state(&g_rix, &g_state);

    if (state->depth_bits == 16U || state->depth_bits == 24U)
        g_depth_bits = (uint8_t)state->depth_bits;
    else if (state->depth_bits == 32U && minigl_rix_backend_available(&g_rix))
        g_depth_bits = 16U; /* RasterIX Z16; forced CPU retains its 24-bit path. */
    else {
        g_error |= RASTER_ERR_STATE;
        g_depth_bits = 24U;
    }
    g_polygon_offset_units = g_state.polygon_offset_units / (float)depth_max();
    g_polygon_depth_scale = ((float)g_state.depth_far - (float)g_state.depth_near) / 16777215.0f;
    if (pixels() > g_buffers.depth_bytes / depth_bytes_per_pixel())
        g_error |= RASTER_ERR_MEMORY;
    /* State is sent before selecting the CPU representation: RasterIX accepts
     * 24/32 requests as Z16, while a forced CPU context remains unchanged. */
    if (!g_context_backend_logged) {
        g_context_backend_logged = 1;
        minigl_rix_backend_log_context_backend(&g_rix);
    }
}

void minigl_raster_load_matrix(uint16_t mode, const float *matrix)
{
    if (!mode_ok(mode) || !matrix) {
        g_error |= RASTER_ERR_MATRIX;
        return;
    }
    memcpy(g_matrix[mode], matrix, sizeof(g_matrix[mode]));
    if (mode < 2U)
        update_mvp();
    if (mode == 0U)
        minigl_lighting_modelview(g_matrix[0]);
}

void minigl_raster_multiply_matrix(uint16_t mode, const float *matrix)
{
    if (!mode_ok(mode) || !matrix) {
        g_error |= RASTER_ERR_MATRIX;
        return;
    }
    matrix_mul(g_matrix[mode], g_matrix[mode], matrix);
    if (mode < 2U)
        update_mvp();
    if (mode == 0U)
        minigl_lighting_modelview(g_matrix[0]);
}

int minigl_raster_push_matrix(uint16_t mode)
{
    if (!mode_ok(mode) || g_stack_depth[mode] >= MATRIX_STACK_DEPTH) {
        g_error |= RASTER_ERR_MATRIX;
        return 0;
    }
    memcpy(g_stack[mode][g_stack_depth[mode]++], g_matrix[mode], sizeof(g_matrix[mode]));
    return 1;
}

int minigl_raster_pop_matrix(uint16_t mode)
{
    if (!mode_ok(mode) || !g_stack_depth[mode]) {
        g_error |= RASTER_ERR_MATRIX;
        return 0;
    }
    memcpy(g_matrix[mode], g_stack[mode][--g_stack_depth[mode]], sizeof(g_matrix[mode]));
    if (mode < 2U)
        update_mvp();
    if (mode == 0U)
        minigl_lighting_modelview(g_matrix[0]);
    return 1;
}

void minigl_raster_bind_texture(uint16_t unit, uint16_t target, uint32_t id)
{
    if (unit >= MAX_TMU || target != GL_TEXTURE_2D) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    g_bound_texture[unit] = id;
    g_bound_texture_info[unit] = texture_create(id);
    if (unit == 0U) minigl_rix_backend_bind_texture(&g_rix, id);
}

void minigl_raster_upload_texture(uint32_t id, uint16_t level, uint16_t width, uint16_t height,
                                  uint16_t format, uint16_t slot, uint32_t size_bytes)
{
    Texture *texture_info;
    TextureLevel *image;
    const uint8_t *source;
    uint64_t texels;
    uint64_t raw_size;
    uint64_t output_size;
    uint64_t staging_offset;
    uint32_t pool_offset;
    uint32_t index;
    uint16_t storage_format = format & ZZ9K_MGL_TEX_STORAGE_MASK;

    if (!ready() || !width || !height || level >= ZZ9K_MINIGL_MAX_MIP_LEVELS) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    texels = (uint64_t)width * height;
    switch (storage_format) {
    case ZZ9K_MGL_TEXFMT_RGBA8888: raw_size = texels * 4U; break;
    case ZZ9K_MGL_TEXFMT_RGB565:
    case ZZ9K_MGL_TEXFMT_ARGB1555:
    case ZZ9K_MGL_TEXFMT_LUM_A8: raw_size = texels * 2U; break;
    case ZZ9K_MGL_TEXFMT_RGB888: raw_size = texels * 3U; break;
    case ZZ9K_MGL_TEXFMT_LUM8: raw_size = texels; break;
    case ZZ9K_MGL_TEXFMT_P8: raw_size = 1024U + texels; break;
    default:
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    output_size = texels * sizeof(uint32_t);
    staging_offset = (uint64_t)slot * MINIGL_RASTER_STAGING_SLOT_BYTES;
    if (raw_size != size_bytes || raw_size > UINT32_MAX || output_size > UINT32_MAX ||
        staging_offset > g_buffers.texture_staging_bytes ||
        raw_size > (uint64_t)g_buffers.texture_staging_bytes - staging_offset ||
        !texture_pool_ensure((uint32_t)output_size, g_texture_used)) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    texture_info = texture_create(id);
    image = texture_info ? &texture_info->levels[level] : NULL;
    if (!image || !texture_alloc(image, (uint32_t)output_size, &pool_offset)) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    source = g_buffers.texture_staging + (uint32_t)staging_offset;
    for (index = 0U; index < (uint32_t)texels; ++index)
        ((uint32_t *)(g_buffers.texture_pool + pool_offset))[index] = unpack_texel(storage_format, source, index);
    image->width = width;
    image->height = height;
    image->format = format & ZZ9K_MGL_TEX_INTERNAL_RGB;
    image->internal_rgb = (format & ZZ9K_MGL_TEX_INTERNAL_RGB) != 0U;
    texture_complete(texture_info);
    if (image->internal_rgb)
        for (index = 0U; index < (uint32_t)texels; ++index)
            ((uint32_t *)(g_buffers.texture_pool + pool_offset))[index] |= 0xffU;
    { unsigned unit; for (unit = 0U; unit < MAX_TMU; ++unit) if (g_bound_texture[unit] == id) g_bound_texture_info[unit] = texture_info; }
    if (!minigl_rix_backend_upload_texture(&g_rix, id, level,
                                           (const uint32_t *)(g_buffers.texture_pool + pool_offset),
                                           width, height, format)) {
        /* CPU residency remains valid; this draw will take the ordered fallback. */
        minigl_rix_backend_bind_texture(&g_rix, 0U);
    } else {
        minigl_rix_backend_tex_params(&g_rix, id, texture_info->min_filter,
                                      texture_info->mag_filter, texture_info->wrap_s,
                                      texture_info->wrap_t);
    }
}

void minigl_raster_tex_subimage(uint32_t id, uint16_t level, uint16_t xoffset, uint16_t yoffset,
                                uint16_t width, uint16_t height, uint16_t format,
                                uint16_t slot, uint32_t size_bytes)
{
    Texture *texture_info = texture(id);
    TextureLevel *image = texture_info && level < ZZ9K_MINIGL_MAX_MIP_LEVELS ? &texture_info->levels[level] : NULL;
    const uint8_t *source;
    uint64_t texels;
    uint64_t raw_size;
    uint64_t staging_offset;
    uint32_t x;
    uint32_t y;
    uint16_t storage_format = format & ZZ9K_MGL_TEX_STORAGE_MASK;

    if (!ready() || !image || !image->size || !width || !height ||
        (uint32_t)xoffset + width > image->width ||
        (uint32_t)yoffset + height > image->height) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    texels = (uint64_t)width * height;
    switch (storage_format) {
    case ZZ9K_MGL_TEXFMT_RGBA8888: raw_size = texels * 4U; break;
    case ZZ9K_MGL_TEXFMT_RGB565:
    case ZZ9K_MGL_TEXFMT_ARGB1555:
    case ZZ9K_MGL_TEXFMT_LUM_A8: raw_size = texels * 2U; break;
    case ZZ9K_MGL_TEXFMT_RGB888: raw_size = texels * 3U; break;
    case ZZ9K_MGL_TEXFMT_LUM8: raw_size = texels; break;
    case ZZ9K_MGL_TEXFMT_P8: raw_size = 1024U + texels; break;
    default:
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    staging_offset = (uint64_t)slot * MINIGL_RASTER_STAGING_SLOT_BYTES;
    if (raw_size != size_bytes || raw_size > UINT32_MAX ||
        staging_offset > g_buffers.texture_staging_bytes ||
        raw_size > (uint64_t)g_buffers.texture_staging_bytes - staging_offset) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    source = g_buffers.texture_staging + (uint32_t)staging_offset;
    for (y = 0U; y < height; ++y) {
        for (x = 0U; x < width; ++x) {
            uint32_t source_index = y * width + x;
            uint32_t destination_index = ((uint32_t)yoffset + y) * image->width + xoffset + x;
            ((uint32_t *)(g_buffers.texture_pool + image->offset))[destination_index] =
                unpack_texel(storage_format, source, source_index) | (image->internal_rgb ? 0xffU : 0U);
        }
    }
    if (!minigl_rix_backend_upload_texture(&g_rix, id, level,
                                           (const uint32_t *)(g_buffers.texture_pool +
                                           image->offset), image->width, image->height,
                                           image->format)) {
        minigl_rix_backend_delete_texture(&g_rix, id);
    }
}

void minigl_raster_tex_params(uint32_t id, uint16_t min_filter, uint16_t mag_filter,
                              uint16_t wrap_s, uint16_t wrap_t)
{
    Texture *texture_info = texture_create(id);
    if (!texture_info ||
        (min_filter != GL_NEAREST && min_filter != GL_LINEAR && !mip_filter(min_filter)) ||
        (mag_filter != GL_NEAREST && mag_filter != GL_LINEAR) ||
        (wrap_s != GL_REPEAT && wrap_s != GL_CLAMP && wrap_s != GL_CLAMP_TO_EDGE) ||
        (wrap_t != GL_REPEAT && wrap_t != GL_CLAMP && wrap_t != GL_CLAMP_TO_EDGE)) {
        g_error |= RASTER_ERR_TEXTURE;
        return;
    }
    texture_info->min_filter = min_filter;
    texture_info->mag_filter = mag_filter;
    texture_info->wrap_s = wrap_s;
    texture_info->wrap_t = wrap_t;
    minigl_rix_backend_tex_params(&g_rix, id, min_filter, mag_filter, wrap_s, wrap_t);
}

void minigl_raster_delete_texture(uint32_t id)
{
    Texture *texture_info;
    unsigned index;

    if (!id) return;
    texture_info = texture(id);
    if (texture_info) {
        for (index = 0U; index < ZZ9K_MINIGL_MAX_MIP_LEVELS; ++index) {
            TextureLevel *image = &texture_info->levels[index];
            if (image->size) { texture_unlink(image); g_texture_used -= image->size; }
        }
        texture_index(id, 0U);
        memset(texture_info, 0, sizeof(*texture_info));
        minigl_rix_backend_delete_texture(&g_rix, id);
    }
    for (index = 0U; index < MAX_TMU; ++index)
        if (g_bound_texture[index] == id) {
            g_bound_texture[index] = 0U;
            g_bound_texture_info[index] = &g_default_texture;
        }
}

static int draw_arrays_valid(uint16_t attr_code, uint16_t stride, uint32_t first,
                             uint32_t count, uint32_t base)
{
    uint64_t first_end = (uint64_t)first + count;
    uint64_t last_byte = (uint64_t)base + first_end * stride;

    return ready() && !(attr_code & ~MINIGL_RASTER_ATTR_POSITION_INT32) &&
           stride == MINIGL_RASTER_VERTEX_STRIDE &&
           last_byte <= g_buffers.vertices_bytes;
}
static int rix_mode_supported(uint16_t mode)
{
    return mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP ||
           mode == GL_TRIANGLE_FAN || mode == GL_QUADS ||
           mode == GL_QUAD_STRIP || mode == GL_POLYGON;
}

static int rix_draw(uint16_t mode, uint16_t stride, uint32_t base,
                    const uint8_t *indices, uint32_t first, uint32_t count)
{
    if (g_rix_cpu_frame || !rix_mode_supported(mode) ||
        !minigl_rix_backend_begin_draw(&g_rix, g_buffers.width, g_buffers.height,
                                       (uintptr_t)g_buffers.color, (uintptr_t)g_buffers.depth) ||
        !minigl_rix_backend_prepare_target(&g_rix, g_buffers.color, pixels())) {
        return 0;
    }
    g_rix_collecting = 1;
    assemble(mode, stride, base, indices, first, count);
    if (g_rix_collecting > 0 &&
        minigl_rix_backend_end_draw(&g_rix, (uintptr_t)g_buffers.color,
                                    (uintptr_t)g_buffers.depth)) {
        g_rix_collecting = 0;
        mark_dirty(0, 0, (int)g_buffers.width - 1, (int)g_buffers.height - 1);
        return 1;
    }
    g_rix_collecting = 0;
    (void)minigl_rix_backend_make_cpu_visible(&g_rix, g_buffers.color, pixels());
    return 0;
}


void minigl_raster_draw_arrays(uint16_t mode, uint16_t attr_code, uint16_t vertex_stride,
                               uint32_t first, uint32_t count, uint32_t vertex_base_offset)
{
    if (!draw_arrays_valid(attr_code, vertex_stride, first, count, vertex_base_offset)) {
        g_error |= RASTER_ERR_VERTEX;
        return;
    }
    g_draw_attr_code = attr_code;
    if (!g_context_backend_logged) {
        g_context_backend_logged = 1;
        minigl_rix_backend_log_context_backend(&g_rix);
    }
    if (rix_draw(mode, vertex_stride, vertex_base_offset, NULL, first, count)) {
        ++g_frame_draws_hardware;
    } else {
        uint32_t reason = minigl_rix_backend_eval_fallback(&g_rix);
        ++g_frame_draws_software;
        if (g_frame_fallback_reason == ZZ9K_MGL_FALLBACK_NONE && reason != ZZ9K_MGL_FALLBACK_NONE)
            g_frame_fallback_reason = reason;
        if (reason != ZZ9K_MGL_FALLBACK_NONE && !(g_logged_fallback_mask & (1U << reason))) {
            g_logged_fallback_mask |= (1U << reason);
            minigl_rix_backend_log_fallback(reason);
        }
        if (++g_rix_cpu_fallbacks >= MINIGL_RIX_CPU_FALLBACKS_PER_FRAME)
            g_rix_cpu_frame = 1;
        if (!rix_cpu_visible()) return;
        assemble(mode, vertex_stride, vertex_base_offset, NULL, first, count);
    }
}

void minigl_raster_draw_elements(uint16_t mode, uint16_t attr_code, uint16_t vertex_stride,
                                 uint32_t count, uint32_t vertex_base_offset,
                                 uint32_t index_base_offset)
{
    const uint8_t *indices;
    uint32_t index;

    if (!ready() || (attr_code & ~MINIGL_RASTER_ATTR_POSITION_INT32) ||
        vertex_stride != MINIGL_RASTER_VERTEX_STRIDE ||
        index_base_offset > g_buffers.vertices_bytes ||
        count > (g_buffers.vertices_bytes - index_base_offset) / 2U) {
        g_error |= RASTER_ERR_VERTEX;
        return;
    }
    indices = g_buffers.vertices + index_base_offset;
    for (index = 0U; index < count; ++index) {
        const uint8_t *unused;
        if (!vertex_address(vertex_base_offset, be16(indices + (size_t)index * 2U), vertex_stride, &unused)) {
            g_error |= RASTER_ERR_VERTEX;
            return;
        }
    }
    g_draw_attr_code = attr_code;
    if (!g_context_backend_logged) {
        g_context_backend_logged = 1;
        minigl_rix_backend_log_context_backend(&g_rix);
    }
    if (rix_draw(mode, vertex_stride, vertex_base_offset, indices, 0U, count)) {
        ++g_frame_draws_hardware;
    } else {
        uint32_t reason = minigl_rix_backend_eval_fallback(&g_rix);
        ++g_frame_draws_software;
        if (g_frame_fallback_reason == ZZ9K_MGL_FALLBACK_NONE && reason != ZZ9K_MGL_FALLBACK_NONE)
            g_frame_fallback_reason = reason;
        if (reason != ZZ9K_MGL_FALLBACK_NONE && !(g_logged_fallback_mask & (1U << reason))) {
            g_logged_fallback_mask |= (1U << reason);
            minigl_rix_backend_log_fallback(reason);
        }
        if (++g_rix_cpu_fallbacks >= MINIGL_RIX_CPU_FALLBACKS_PER_FRAME)
            g_rix_cpu_frame = 1;
        if (!rix_cpu_visible()) return;
        assemble(mode, vertex_stride, vertex_base_offset, indices, 0U, count);
    }
}

void minigl_raster_present(const ZZ9KMiniGLPacketPresent *present)
{
    g_last_frame_draws_hardware = g_frame_draws_hardware;
    g_last_frame_draws_software = g_frame_draws_software;
    g_last_frame_fallback_reason = g_frame_fallback_reason;
    g_frame_draws_hardware = 0U;
    g_frame_draws_software = 0U;
    g_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    g_rix_cpu_fallbacks = 0U;
    g_rix_cpu_frame = 0;
    if (!present || !ready() ||
        (minigl_rix_backend_surface_is_rgb565(&g_rix) ?
         (!minigl_rix_backend_drain(&g_rix) ||
          !minigl_present_frame_rgb565(present, (const uint16_t *)g_buffers.color)) :
         !minigl_present_frame(present, g_buffers.color))) {
        g_error |= RASTER_ERR_MEMORY;
    }
}
uint32_t minigl_raster_draws_hardware(void)
{
    return (g_last_frame_draws_hardware || g_last_frame_draws_software) ?
        g_last_frame_draws_hardware : g_frame_draws_hardware;
}

uint32_t minigl_raster_draws_software(void)
{
    return (g_last_frame_draws_hardware || g_last_frame_draws_software) ?
        g_last_frame_draws_software : g_frame_draws_software;
}

uint32_t minigl_raster_first_fallback_reason(void)
{
    return (g_last_frame_draws_hardware || g_last_frame_draws_software) ?
        g_last_frame_fallback_reason : g_frame_fallback_reason;
}

int minigl_raster_hardware_available(void)
{
    return minigl_rix_backend_available(&g_rix);
}

int minigl_raster_get_rix_diagnostics(MiniGLRasterRixDiagnostics *out)
{
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!g_rix.available)
        return 0;
    out->active = (uint8_t)minigl_rix_backend_available(&g_rix);
    out->last_fence = g_rix.driver.submitted_target;
    out->status = g_rix.platform.csr_read ?
        g_rix.platform.csr_read(g_rix.platform.context, RIX_DRV_CSR_STATUS) :
        g_rix.driver.last_status;
    out->completed_fence = g_rix.platform.csr_read ?
        g_rix.platform.csr_read(g_rix.platform.context, RIX_DRV_CSR_COMPLETED) : 0U;
    return 1;
}

void minigl_raster_reset_frame_counters(void)
{
    g_frame_draws_hardware = 0U;
    g_frame_draws_software = 0U;
    g_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    g_last_frame_draws_hardware = 0U;
    g_last_frame_draws_software = 0U;
    g_last_frame_fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
}

int minigl_raster_upload_rgba8(uint32_t id, uint16_t level, uint16_t width, uint16_t height,
                               const uint32_t *rgba_pixels, int internal_rgb)
{
    Texture *texture_info;
    TextureLevel *image;
    uint32_t pool_offset;
    uint64_t texels;
    uint64_t output_size;
    uint32_t index;

    if (!ready() || !width || !height || level >= ZZ9K_MINIGL_MAX_MIP_LEVELS) {
        g_error |= RASTER_ERR_TEXTURE;
        return 0;
    }
    texels = (uint64_t)width * height;
    output_size = texels * sizeof(uint32_t);
    if (!texture_pool_ensure((uint32_t)output_size, g_texture_used)) {
        g_error |= RASTER_ERR_TEXTURE;
        return 0;
    }
    texture_info = texture_create(id);
    image = texture_info ? &texture_info->levels[level] : NULL;
    if (!image || !texture_alloc(image, (uint32_t)output_size, &pool_offset)) {
        g_error |= RASTER_ERR_TEXTURE;
        return 0;
    }
    if (rgba_pixels != NULL) {
        memcpy(g_buffers.texture_pool + pool_offset, rgba_pixels, (size_t)output_size);
        if (internal_rgb) {
            uint32_t *dst = (uint32_t *)(g_buffers.texture_pool + pool_offset);
            for (index = 0U; index < (uint32_t)texels; ++index)
                dst[index] |= 0xffU;
        }
    } else {
        memset(g_buffers.texture_pool + pool_offset, 0, (size_t)output_size);
    }
    image->width = width;
    image->height = height;
    image->format = internal_rgb ? ZZ9K_MGL_TEX_INTERNAL_RGB : 0U;
    image->internal_rgb = (uint8_t)(internal_rgb ? 1U : 0U);
    texture_complete(texture_info);
    {
        unsigned unit;
        for (unit = 0U; unit < MAX_TMU; ++unit) {
            if (g_bound_texture[unit] == id)
                g_bound_texture_info[unit] = texture_info;
        }
    }
    (void)minigl_rix_backend_upload_texture(&g_rix, id, level,
                                            (const uint32_t *)(g_buffers.texture_pool + pool_offset),
                                            width, height, image->format);
    return 1;
}

int minigl_raster_subimage_rgba8(uint32_t id, uint16_t level, uint16_t xoffset, uint16_t yoffset,
                                 uint16_t width, uint16_t height, const uint32_t *rgba_pixels,
                                 int internal_rgb)
{
    Texture *texture_info = texture(id);
    TextureLevel *image = (texture_info && level < ZZ9K_MINIGL_MAX_MIP_LEVELS) ?
                          &texture_info->levels[level] : NULL;
    uint32_t *dest;
    uint32_t y;

    if (!ready() || !image || !image->size || !rgba_pixels) {
        g_error |= RASTER_ERR_TEXTURE;
        return 0;
    }
    if (xoffset + width > image->width || yoffset + height > image->height) {
        g_error |= RASTER_ERR_TEXTURE;
        return 0;
    }
    dest = (uint32_t *)(g_buffers.texture_pool + image->offset);
    for (y = 0; y < height; ++y) {
        uint32_t *dst_row = dest + (size_t)(yoffset + y) * image->width + xoffset;
        const uint32_t *src_row = rgba_pixels + (size_t)y * width;
        if (internal_rgb) {
            uint32_t x;
            for (x = 0; x < width; ++x)
                dst_row[x] = src_row[x] | 0xffU;
        } else {
            memcpy(dst_row, src_row, (size_t)width * sizeof(uint32_t));
        }
    }
    (void)minigl_rix_backend_upload_texture(&g_rix, id, level, dest,
                                           image->width, image->height, image->format);
    return 1;
}

static void sphere_map_coords(const float eye[4], const float normal_eye[3], float out_st[2])
{
    out_st[0] = 0.5f;
    out_st[1] = 0.5f;
    float normal_len = sqrtf(normal_eye[0]*normal_eye[0] + normal_eye[1]*normal_eye[1] + normal_eye[2]*normal_eye[2]);
    float eye_len = sqrtf(eye[0]*eye[0] + eye[1]*eye[1] + eye[2]*eye[2]);
    if (normal_len < 1e-7f || eye_len < 1e-7f) return;
    float n[3] = { normal_eye[0] / normal_len, normal_eye[1] / normal_len, normal_eye[2] / normal_len };
    float u[3] = { eye[0] / eye_len, eye[1] / eye_len, eye[2] / eye_len };
    float d = u[0]*n[0] + u[1]*n[1] + u[2]*n[2];
    float r[3] = { u[0] - 2.0f * d * n[0], u[1] - 2.0f * d * n[1], u[2] - 2.0f * d * n[2] };
    float divisor = 2.0f * sqrtf(r[0]*r[0] + r[1]*r[1] + (r[2] + 1.0f)*(r[2] + 1.0f));
    if (divisor < 1e-7f) return;
    out_st[0] += r[0] / divisor;
    out_st[1] += r[1] / divisor;
}

static void v9_vert_convert(const float *v, Vertex *out)
{
    float clip[4];
    transform_mvp(g_mvp, v[0], v[1], v[2], v[3], clip);
    out->x = clip[0];
    out->y = clip[1];
    out->z = clip[2];
    out->w = clip[3];

    float eye[4];
    const float *mv = g_matrix[0];
    eye[0] = v[0] * mv[0] + v[1] * mv[4] + v[2] * mv[8] + v[3] * mv[12];
    eye[1] = v[0] * mv[1] + v[1] * mv[5] + v[2] * mv[9] + v[3] * mv[13];
    eye[2] = v[0] * mv[2] + v[1] * mv[6] + v[2] * mv[10] + v[3] * mv[14];
    eye[3] = v[0] * mv[3] + v[1] * mv[7] + v[2] * mv[11] + v[3] * mv[15];

    out->fog_coord = (eye[3] != 0.0f) ? fabsf(eye[2] / eye[3]) : 0.0f;

    float normal_eye[3];
    if (g_mw_state) {
        const float *nm = g_mw_state->normal_matrix;
        normal_eye[0] = nm[0] * v[16] + nm[3] * v[17] + nm[6] * v[18];
        normal_eye[1] = nm[1] * v[16] + nm[4] * v[17] + nm[7] * v[18];
        normal_eye[2] = nm[2] * v[16] + nm[5] * v[17] + nm[8] * v[18];
    } else {
        normal_eye[0] = v[16];
        normal_eye[1] = v[17];
        normal_eye[2] = v[18];
    }

    if (g_mw_state && g_mw_state->lighting_enabled) {
        minigl_lighting_shade_wire_v9(eye, normal_eye, &v[4], &out->front_color, &out->back_color);
    } else {
        uint8_t r = (uint8_t)(clamp01(v[4]) * 255.0f + 0.5f);
        uint8_t g = (uint8_t)(clamp01(v[5]) * 255.0f + 0.5f);
        uint8_t b = (uint8_t)(clamp01(v[6]) * 255.0f + 0.5f);
        uint8_t a = (uint8_t)(clamp01(v[7]) * 255.0f + 0.5f);
        out->front_color = rgba(r, g, b, a);
        out->back_color = out->front_color;
    }
    out->color = out->front_color;
    out->flat_color = out->front_color;

    float sphere[2];
    int sphere_computed = 0;

    /* Unit 0 */
    float tc0[4] = { v[8], v[9], v[10], v[11] };
    if (g_mw_state && (g_mw_state->texgen[0].s_enabled || g_mw_state->texgen[0].t_enabled)) {
        if (g_mw_state->texgen[0].s_enabled) {
            uint32_t mode = g_mw_state->texgen[0].s_mode;
            if (mode == 0x2400)
                tc0[0] = g_mw_state->texgen[0].eye_plane_s[0]*eye[0] + g_mw_state->texgen[0].eye_plane_s[1]*eye[1] +
                         g_mw_state->texgen[0].eye_plane_s[2]*eye[2] + g_mw_state->texgen[0].eye_plane_s[3]*eye[3];
            else if (mode == 0x2401)
                tc0[0] = g_mw_state->texgen[0].obj_plane_s[0]*v[0] + g_mw_state->texgen[0].obj_plane_s[1]*v[1] +
                         g_mw_state->texgen[0].obj_plane_s[2]*v[2] + g_mw_state->texgen[0].obj_plane_s[3]*v[3];
            else if (mode == 0x2402) {
                if (!sphere_computed) { sphere_map_coords(eye, normal_eye, sphere); sphere_computed = 1; }
                tc0[0] = sphere[0];
            }
        }
        if (g_mw_state->texgen[0].t_enabled) {
            uint32_t mode = g_mw_state->texgen[0].t_mode;
            if (mode == 0x2400)
                tc0[1] = g_mw_state->texgen[0].eye_plane_t[0]*eye[0] + g_mw_state->texgen[0].eye_plane_t[1]*eye[1] +
                         g_mw_state->texgen[0].eye_plane_t[2]*eye[2] + g_mw_state->texgen[0].eye_plane_t[3]*eye[3];
            else if (mode == 0x2401)
                tc0[1] = g_mw_state->texgen[0].obj_plane_t[0]*v[0] + g_mw_state->texgen[0].obj_plane_t[1]*v[1] +
                         g_mw_state->texgen[0].obj_plane_t[2]*v[2] + g_mw_state->texgen[0].obj_plane_t[3]*v[3];
            else if (mode == 0x2402) {
                if (!sphere_computed) { sphere_map_coords(eye, normal_eye, sphere); sphere_computed = 1; }
                tc0[1] = sphere[1];
            }
        }
    }
    out->u0 = tc0[0];
    out->v0 = tc0[1];
    out->q0 = tc0[3];
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0)
        texture_matrix(0U, &out->u0, &out->v0, tc0[2], &out->q0);

    /* Unit 1 */
    float tc1[4] = { v[12], v[13], v[14], v[15] };
    if (g_mw_state && (g_mw_state->texgen[1].s_enabled || g_mw_state->texgen[1].t_enabled)) {
        if (g_mw_state->texgen[1].s_enabled) {
            uint32_t mode = g_mw_state->texgen[1].s_mode;
            if (mode == 0x2400)
                tc1[0] = g_mw_state->texgen[1].eye_plane_s[0]*eye[0] + g_mw_state->texgen[1].eye_plane_s[1]*eye[1] +
                         g_mw_state->texgen[1].eye_plane_s[2]*eye[2] + g_mw_state->texgen[1].eye_plane_s[3]*eye[3];
            else if (mode == 0x2401)
                tc1[0] = g_mw_state->texgen[1].obj_plane_s[0]*v[0] + g_mw_state->texgen[1].obj_plane_s[1]*v[1] +
                         g_mw_state->texgen[1].obj_plane_s[2]*v[2] + g_mw_state->texgen[1].obj_plane_s[3]*v[3];
            else if (mode == 0x2402) {
                if (!sphere_computed) { sphere_map_coords(eye, normal_eye, sphere); sphere_computed = 1; }
                tc1[0] = sphere[0];
            }
        }
        if (g_mw_state->texgen[1].t_enabled) {
            uint32_t mode = g_mw_state->texgen[1].t_mode;
            if (mode == 0x2400)
                tc1[1] = g_mw_state->texgen[1].eye_plane_t[0]*eye[0] + g_mw_state->texgen[1].eye_plane_t[1]*eye[1] +
                         g_mw_state->texgen[1].eye_plane_t[2]*eye[2] + g_mw_state->texgen[1].eye_plane_t[3]*eye[3];
            else if (mode == 0x2401)
                tc1[1] = g_mw_state->texgen[1].obj_plane_t[0]*v[0] + g_mw_state->texgen[1].obj_plane_t[1]*v[1] +
                         g_mw_state->texgen[1].obj_plane_t[2]*v[2] + g_mw_state->texgen[1].obj_plane_t[3]*v[3];
            else if (mode == 0x2402) {
                if (!sphere_computed) { sphere_map_coords(eye, normal_eye, sphere); sphere_computed = 1; }
                tc1[1] = sphere[1];
            }
        }
    }
    out->u1 = tc1[0];
    out->v1 = tc1[1];
    out->q1 = tc1[3];
    if (g_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1)
        texture_matrix(1U, &out->u1, &out->v1, tc1[2], &out->q1);
}

static int v9_mask_edge(const uint32_t *mask, size_t i)
{
    /* The wire carries the edge-mask words big-endian (mw_draw.c's
     * mw_mask_edge reads them the same way). */
    if (!mask) return 1;
    return (mw_be32((const uint8_t *)mask + (i / 32) * 4) & (1U << (i % 32))) != 0;
}

static void assemble_wire_v9(uint16_t mode, const float *object_vertices,
                             uint32_t count, uint32_t flags,
                             const uint32_t *edge_mask)
{
    uint32_t i;
    Vertex a, b, c, d;

    if (flags != 0 && count >= 3 && mode >= GL_TRIANGLES) {
        float area = mw_clipped_area(g_mvp, (float)g_state.viewport_w, (float)g_state.viewport_h,
                                     object_vertices, object_vertices + MW_OBJECT_WORDS,
                                     object_vertices + 2 * MW_OBJECT_WORDS);
        int ccw = area >= 0.0f;
        int front = (g_state.front_face == 0x0900U /* GL_CW */) ? !ccw : ccw;
        unsigned style = (flags >> (front ? MW_LOWERED_FRONT_SHIFT : MW_LOWERED_BACK_SHIFT)) & 3U;

        if (style == 1U) {
            for (i = 0; i < count; ++i) {
                v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                primitive_point(&a);
            }
            return;
        }

        if (style == 2U) {
            /* Outline: emit the primitive's boundary edges as independent
             * lines, exactly as the reference lowers them (one GL_LINES
             * pair per kept edge, edge mask applied per leading vertex).
             * Vertices are converted per edge -- the lowered chunk ceiling
             * (3443) is far above any stack buffer, and a vertex joins at
             * most three edges, so no count falls back to fill here. */
            if (mode == GL_TRIANGLES) {
                for (i = 0; i + 2 < count; i += 3) {
                    if (v9_mask_edge(edge_mask, i)) {
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 1) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 1)) {
                        v9_vert_convert(object_vertices + (i + 1) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 2) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 2)) {
                        v9_vert_convert(object_vertices + (i + 2) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                }
            } else if (mode == GL_TRIANGLE_STRIP) {
                for (i = 2; i < count; ++i) {
                    uint32_t p = i - 2, q = i - 1;
                    if (i & 1) { uint32_t t = p; p = q; q = t; }
                    if (v9_mask_edge(edge_mask, p)) {
                        v9_vert_convert(object_vertices + p * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + q * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, q)) {
                        v9_vert_convert(object_vertices + q * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i)) {
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + p * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                }
            } else if (mode == GL_TRIANGLE_FAN) {
                for (i = 2; i < count; ++i) {
                    if (v9_mask_edge(edge_mask, 0)) {
                        v9_vert_convert(object_vertices, &a);
                        v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i - 1)) {
                        v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i)) {
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices, &b);
                        primitive_line(&a, &b);
                    }
                }
            } else if (mode == GL_QUADS) {
                for (i = 0; i + 3 < count; i += 4) {
                    if (v9_mask_edge(edge_mask, i)) {
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 1) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 1)) {
                        v9_vert_convert(object_vertices + (i + 1) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 2) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 2)) {
                        v9_vert_convert(object_vertices + (i + 2) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 3) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 3)) {
                        v9_vert_convert(object_vertices + (i + 3) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                }
            } else if (mode == GL_QUAD_STRIP) {
                for (i = 0; i + 3 < count; i += 2) {
                    if (v9_mask_edge(edge_mask, i)) {
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 1) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 1)) {
                        v9_vert_convert(object_vertices + (i + 1) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 3) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 3)) {
                        v9_vert_convert(object_vertices + (i + 3) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + (i + 2) * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                    if (v9_mask_edge(edge_mask, i + 2)) {
                        v9_vert_convert(object_vertices + (i + 2) * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                }
            } else if (mode == GL_POLYGON) {
                for (i = 0; i < count; ++i) {
                    uint32_t next = (i + 1 == count) ? 0 : i + 1;
                    if (v9_mask_edge(edge_mask, i)) {
                        v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
                        v9_vert_convert(object_vertices + next * MW_OBJECT_WORDS, &b);
                        primitive_line(&a, &b);
                    }
                }
            }
            return;
        }

        if (flags & MW_LOWERED_FLAT) {
            if (mode == GL_TRIANGLES) {
                for (i = 2; i < count; i += 3) {
                    v9_vert_convert(object_vertices + (i - 2) * MW_OBJECT_WORDS, &a);
                    v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &b);
                    v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &c);
                    a.flat_color = b.flat_color = c.flat_color = c.color;
                    primitive_triangle(&a, &b, &c);
                }
                return;
            }
        }
    }

    if (mode == GL_POINTS) {
        for (i = 0; i < count; ++i) {
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &a);
            primitive_point(&a);
        }
        return;
    }

    if (mode == GL_LINES) {
        for (i = 1; i < count; i += 2) {
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
            primitive_line(&a, &b);
        }
        return;
    }

    if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
        for (i = 1; i < count; ++i) {
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &b);
            primitive_line(&a, &b);
        }
        if (mode == GL_LINE_LOOP && count > 2) {
            v9_vert_convert(object_vertices + (count - 1) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices, &b);
            primitive_line(&a, &b);
        }
        return;
    }

    if (mode == GL_TRIANGLES) {
        for (i = 2; i < count; i += 3) {
            v9_vert_convert(object_vertices + (i - 2) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &b);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &c);
            primitive_triangle(&a, &b, &c);
        }
        return;
    }

    if (mode == GL_TRIANGLE_STRIP) {
        for (i = 2; i < count; ++i) {
            v9_vert_convert(object_vertices + (i - 2) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &b);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &c);
            if (i & 1)
                primitive_triangle(&b, &a, &c);
            else
                primitive_triangle(&a, &b, &c);
        }
        return;
    }

    if (mode == GL_TRIANGLE_FAN) {
        v9_vert_convert(object_vertices, &a);
        for (i = 2; i < count; ++i) {
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &b);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &c);
            primitive_triangle(&a, &b, &c);
        }
        return;
    }

    if (mode == GL_QUADS) {
        /* A lowered quad is expanded into two triangles before clipping,
         * mirroring the reference's guest-lowering (expand_lowered). */
        for (i = 3; i < count; i += 4) {
            v9_vert_convert(object_vertices + (i - 3) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices + (i - 2) * MW_OBJECT_WORDS, &b);
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &c);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &d);
            primitive_triangle(&a, &b, &c);
            primitive_triangle(&a, &c, &d);
        }
        return;
    }

    if (mode == GL_QUAD_STRIP) {
        for (i = 3; i < count; i += 2) {
            v9_vert_convert(object_vertices + (i - 3) * MW_OBJECT_WORDS, &a);
            v9_vert_convert(object_vertices + (i - 2) * MW_OBJECT_WORDS, &b);
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &c);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &d);
            primitive_triangle(&a, &b, &d);
            primitive_triangle(&a, &d, &c);
        }
        return;
    }

    if (mode == GL_POLYGON) {
        /* A lowered polygon is a fan from vertex 0, as the reference
         * lowers it; the whole-polygon clip path stays with the legacy
         * consumer ABI. */
        v9_vert_convert(object_vertices, &a);
        for (i = 2; i < count; ++i) {
            v9_vert_convert(object_vertices + (i - 1) * MW_OBJECT_WORDS, &b);
            v9_vert_convert(object_vertices + i * MW_OBJECT_WORDS, &c);
            primitive_triangle(&a, &b, &c);
        }
        return;
    }
}

void minigl_raster_draw_wire_v9(uint16_t mode, const float *object_vertices,
                                uint32_t count, uint32_t flags,
                                const uint32_t *edge_mask)
{
    unsigned style = 0;

    if (!ready() || count == 0 || !object_vertices)
        return;

    if (!g_context_backend_logged) {
        g_context_backend_logged = 1;
        minigl_rix_backend_log_context_backend(&g_rix);
    }

    if (flags != 0 && count >= 3 && mode >= GL_TRIANGLES) {
        float area = mw_clipped_area(g_mvp, (float)g_state.viewport_w, (float)g_state.viewport_h,
                                     object_vertices, object_vertices + MW_OBJECT_WORDS,
                                     object_vertices + 2 * MW_OBJECT_WORDS);
        int ccw = area >= 0.0f;
        int front = (g_state.front_face == 0x0900U /* GL_CW */) ? !ccw : ccw;
        style = (flags >> (front ? MW_LOWERED_FRONT_SHIFT : MW_LOWERED_BACK_SHIFT)) & 3U;
    }

    if (!g_rix_cpu_frame && style == 0U && rix_mode_supported(mode) &&
        minigl_rix_backend_available(&g_rix) &&
        minigl_rix_backend_begin_draw(&g_rix, g_buffers.width, g_buffers.height,
                                      (uintptr_t)g_buffers.color, (uintptr_t)g_buffers.depth) &&
        minigl_rix_backend_prepare_target(&g_rix, g_buffers.color, pixels())) {

        g_rix_collecting = 1;
        assemble_wire_v9(mode, object_vertices, count, flags, edge_mask);
        if (g_rix_collecting > 0 &&
            minigl_rix_backend_end_draw(&g_rix, (uintptr_t)g_buffers.color,
                                        (uintptr_t)g_buffers.depth)) {
            g_rix_collecting = 0;
            ++g_frame_draws_hardware;
            mark_dirty(0, 0, (int)g_buffers.width - 1, (int)g_buffers.height - 1);
            return;
        }
        g_rix_collecting = 0;
        (void)minigl_rix_backend_make_cpu_visible(&g_rix, g_buffers.color, pixels());
    }

    /* Fallback to software rasterizer */
    uint32_t reason = (style != 0U) ? ZZ9K_MGL_FALLBACK_POLYGON_MODE
                                    : minigl_rix_backend_eval_fallback(&g_rix);
    ++g_frame_draws_software;
    if (g_frame_fallback_reason == ZZ9K_MGL_FALLBACK_NONE && reason != ZZ9K_MGL_FALLBACK_NONE)
        g_frame_fallback_reason = reason;
    if (reason != ZZ9K_MGL_FALLBACK_NONE && !(g_logged_fallback_mask & (1U << reason))) {
        g_logged_fallback_mask |= (1U << reason);
        minigl_rix_backend_log_fallback(reason);
    }
    if (++g_rix_cpu_fallbacks >= MINIGL_RIX_CPU_FALLBACKS_PER_FRAME)
        g_rix_cpu_frame = 1;
    (void)minigl_raster_finish();
    g_rix_collecting = 0;
    assemble_wire_v9(mode, object_vertices, count, flags, edge_mask);
}
