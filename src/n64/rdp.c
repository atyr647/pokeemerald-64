/*
 * src/n64/rdp.c
 *
 * N64 port — Reality Display Processor command-list builder
 *
 * See include/n64/rdp.h for why this talks to the DP directly rather than
 * through RSP microcode, and for the source of the opcode/field layouts.
 *
 * The command list lives in a fixed 16KB RDRAM buffer (n64.ld's .rdp_dlist)
 * and is written through its KSEG1 (uncached) alias -- exactly how vi.c
 * clears the framebuffers -- so a CPU store lands in RDRAM immediately and
 * the DP's own RDRAM read sees it with no cache writeback of the list
 * itself. Submitting means pointing DPC_START/DPC_END at the span and
 * polling DPC_STATUS until the pipe drains; there is no DP interrupt
 * handling yet because nothing here overlaps RDP work with anything else.
 */

#include "global.h"
#include "n64/asm_defs.h"
#include "n64/rdp.h"

#define DP_WR(off, val)  N64_HW_WR(N64_DP_BASE_REG, (off), (u32)(val))
#define DP_RD(off)       N64_HW_RD(N64_DP_BASE_REG, (off))

/* DPC_STATUS write bits (clear/set pairs) */
#define DPC_CLR_XBUS    0x0001u
#define DPC_CLR_FREEZE  0x0004u
#define DPC_CLR_FLUSH   0x0010u

/* DPC_STATUS read bits */
#define DPC_PIPE_BUSY   0x0020u
#define DPC_CMD_BUSY    0x0040u
#define DPC_DMA_BUSY    0x0100u

/* RDP command opcodes (low 6 bits of the top byte of word 0) */
#define RDP_TEXTURE_RECT    0x24u
#define RDP_SYNC_LOAD       0x26u
#define RDP_SYNC_PIPE       0x27u
#define RDP_SYNC_TILE       0x28u
#define RDP_SYNC_FULL       0x29u
#define RDP_SET_SCISSOR     0x2Du
#define RDP_SET_OTHER_MODES 0x2Fu
#define RDP_LOAD_TLUT       0x30u
#define RDP_SET_TILE_SIZE   0x32u
#define RDP_LOAD_TILE       0x34u
#define RDP_SET_TILE        0x35u
#define RDP_FILL_RECT       0x36u
#define RDP_SET_FILL_COLOR  0x37u
#define RDP_SET_COMBINE     0x3Cu
#define RDP_SET_TEX_IMAGE   0x3Du
#define RDP_SET_COLOR_IMAGE 0x3Fu

/* Cycle types, SET_OTHER_MODES bits 53:52 (word 0 bits 21:20). */
#define CYCLE_1CYCLE 0u
#define CYCLE_COPY   2u
#define CYCLE_FILL   3u

extern u8 __rdp_dlist_start[];
extern u8 __rdp_dlist_end[];

/* The list is split into two halves, one written while the other is (or
 * was) what the DP is reading, and each submit toggles which one is
 * current -- the same double-buffering the framebuffers and audio buffers
 * already use, and for the same reason: an RDP that is still consuming
 * one region must never see the CPU start overwriting it for the next
 * batch. It also means two consecutive submits are never the same
 * DPC_START/DPC_END pair, which some RDP implementations need to notice
 * a new batch at all -- see the comment on the trailing SYNC_FULL below. */
#define DL_HALF (((u32)(__rdp_dlist_end - __rdp_dlist_start)) / 2)

static u32 sDlOff;      /* write cursor, bytes from the start of the half */
static u32 sBufIndex;   /* which half is currently being written, 0 or 1  */
static u32 sCycle;      /* the cycle type the last mode setter selected   */
static int sBilerp;      /* the last mode setter turned the texture filter on */

static u32 CurrentBase(void)
{
    return (u32)((uintptr_t)__rdp_dlist_start & 0x00FFFFFFu) + sBufIndex * DL_HALF;
}

/* -----------------------------------------------------------------------
 * Display-list plumbing
 * --------------------------------------------------------------------- */
void RDP_Init(void)
{
    sDlOff = 0;
    sBufIndex = 0;
    sCycle = CYCLE_FILL;
    sBilerp = 0;
    DP_WR(DPC_STATUS_REG, DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH);
}

void RDP_Submit(void)
{
    if (sDlOff == 0)
        return;

    /* Close the batch with SYNC_FULL. Repeating the same DPC_START/END
     * pair across frames -- which a fixed-size ring naturally does once
     * it wraps -- was silently ignored by the RDP the second time it saw
     * it, with DPC_CURRENT already reading as caught-up and the busy bits
     * never set, as if there were nothing new to do. SYNC_FULL is what a
     * real display list always ends a frame with (it is also the DP
     * interrupt's trigger), and appending it here means every caller gets
     * that for free instead of needing to remember it. Written directly
     * rather than through DlCmd() so it can never itself trigger a nested
     * submit. */
    if (sDlOff + 8 <= DL_HALF) {
        volatile u32 *p = (volatile u32 *)((uintptr_t)(__rdp_dlist_start
            + sBufIndex * DL_HALF + sDlOff) | 0x20000000u);
        p[0] = RDP_SYNC_FULL << 24;
        p[1] = 0;
        sDlOff += 8;
    }

    u32 len = sDlOff;
    u32 base = CurrentBase();
    sDlOff = 0;
    sBufIndex ^= 1u;

    DP_WR(DPC_STATUS_REG, DPC_CLR_XBUS | DPC_CLR_FREEZE | DPC_CLR_FLUSH);
    DP_WR(DPC_START_REG, base);
    DP_WR(DPC_END_REG,   base + len);

    while (DP_RD(DPC_STATUS_REG) & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY))
        ;
}

/* Every RDP command is a whole number of 64-bit words; two u32s covers all
 * of the ones this file emits (fills, tiles, rectangles -- nothing here
 * issues a triangle, whose edge/shade/texture blocks run longer). A buffer
 * that would overflow is submitted first: safe, because each command is
 * self-contained once written. */
void RDP_Reserve(int bytes)
{
    if (sDlOff + (u32)bytes + 8 > DL_HALF)
        RDP_Submit();
}

static void DlCmd(u32 w0, u32 w1)
{
    /* Room for this command plus the SYNC_FULL that RDP_Submit() appends. */
    if (sDlOff + 16 > DL_HALF)
        RDP_Submit();
    volatile u32 *p = (volatile u32 *)((uintptr_t)(__rdp_dlist_start
        + sBufIndex * DL_HALF + sDlOff) | 0x20000000u);
    p[0] = w0;
    p[1] = w1;
    sDlOff += 8;
}

/* Screen coordinates are the RDP's 10.2 fixed point, an unsigned 12-bit
 * field. Correct for SET_SCISSOR and the rectangle commands; a triangle's
 * Y coordinates are a *signed* 11.2 field instead, but nothing here draws
 * triangles. */
static u32 ToFx102(int v)
{
    if (v < 0) v = 0;
    if (v > 1023) v = 1023;
    return (u32)v * 4u;
}

/* -----------------------------------------------------------------------
 * Render targets and clipping
 * --------------------------------------------------------------------- */
void RDP_SetColorImage(u32 addr, u32 fmt, u32 size, int width)
{
    u32 wm1 = ((u32)width - 1u) & 0x3FFu;
    u32 w0 = (RDP_SET_COLOR_IMAGE << 24) | (fmt << 21) | (size << 19) | wm1;
    DlCmd(w0, addr & 0x00FFFFFFu);
}

void RDP_SetScissor(int x0, int y0, int x1, int y1)
{
    u32 w0 = (RDP_SET_SCISSOR << 24) | (ToFx102(x0) << 12) | ToFx102(y0);
    u32 w1 = (ToFx102(x1) << 12) | ToFx102(y1);
    DlCmd(w0, w1);
}

/* -----------------------------------------------------------------------
 * Render modes
 * --------------------------------------------------------------------- */
static void SetOtherModes(u32 hi, u32 lo)
{
    DlCmd((RDP_SET_OTHER_MODES << 24) | (hi & 0x00FFFFFFu), lo);
}

static void SetCombine(u32 hi, u32 lo)
{
    DlCmd((RDP_SET_COMBINE << 24) | (hi & 0x00FFFFFFu), lo);
}

void RDP_SyncPipe(void) { DlCmd(RDP_SYNC_PIPE << 24, 0); }
void RDP_SyncTile(void) { DlCmd(RDP_SYNC_TILE << 24, 0); }
void RDP_SyncLoad(void) { DlCmd(RDP_SYNC_LOAD << 24, 0); }
void RDP_SyncFull(void) { DlCmd(RDP_SYNC_FULL << 24, 0); }

/* FILL cycle: the rasterizer writes the fill colour straight out, no
 * combiner or blender in the path at all -- the cheapest way to paint a
 * solid rectangle. */
void RDP_SetModeFill(u16 rgba5551)
{
    RDP_SyncPipe();
    sCycle = CYCLE_FILL;
    sBilerp = 0;
    SetOtherModes(CYCLE_FILL << 20, 0);
    RDP_SetFillColor16(rgba5551);
}

/* COPY cycle: four texels per cycle, no filtering or blending -- the fast
 * path for opaque tile/sprite blits. alpha_compare_en (bit 0 of the low
 * word) drops fully-transparent texels instead of drawing them black. */
void RDP_SetModeCopy(void)
{
    RDP_SyncPipe();
    sCycle = CYCLE_COPY;
    sBilerp = 0;
    SetOtherModes(CYCLE_COPY << 20, 0x00000001u);
}

/* 1-cycle, texture passed straight through the combiner, blended over the
 * framebuffer by source alpha: (tex*A + mem*(1-A)).
 *
 * bi_lerp0/bi_lerp1 (bits 11/10 of the high word) have to be set even
 * though nothing here wants filtering -- leaving them clear puts the
 * texture filter unit in its YUV convert mode instead, which reads an
 * RGBA texture as chroma and returns black. Combiner: (0-0)*0+TEX0 for
 * both colour and alpha, in both cycles.
 *
 * Real filtering is the price: with those bits set the texture unit
 * samples bilinearly, not at a point, and RDP_TextureRectangle has to
 * compensate with a half-texel offset (see there) or every sample lands
 * exactly on a texel edge and blends 50/50 with its neighbour -- which
 * is invisible on a flat fill and severe on anything with sharp detail,
 * which is exactly backwards from a bug you'd catch by eye on a test
 * pattern. sBilerp is what tells RDP_TextureRectangle to add it. */
void RDP_SetModeStandard(void)
{
    RDP_SyncPipe();
    sCycle = CYCLE_1CYCLE;
    sBilerp = 1;
    u32 filt = (1u << 11) | (1u << 10);
    u32 blend = (1u << 22) | (1u << 20);
    SetOtherModes((CYCLE_1CYCLE << 20) | filt, blend | 0x00006040u);
    SetCombine(0x00887F10u, 0x88FCF279u);
}

/* Same as RDP_SetModeStandard, with en_tlut (bit 47 of the 56-bit register,
 * bit 15 of the high word passed to SET_OTHER_MODES) also set so a CI
 * texel is looked up through the palette instead of read as a direct
 * colour. tlut_type (bit 46, bit 14 here) is left clear for RGBA16 --
 * every TLUT this port builds is already-converted RGBA5551, not IA. */
void RDP_SetModeStandardTlut(void)
{
    RDP_SyncPipe();
    sCycle = CYCLE_1CYCLE;
    sBilerp = 1;
    u32 filt = (1u << 11) | (1u << 10);
    u32 blend = (1u << 22) | (1u << 20);
    u32 enTlut = (1u << 15);
    SetOtherModes((CYCLE_1CYCLE << 20) | filt | enTlut, blend | 0x00006040u);
    SetCombine(0x00887F10u, 0x88FCF279u);
}

void RDP_SetFillColor16(u16 rgba5551)
{
    u32 px = rgba5551;
    /* In 16bpp FILL mode the 32-bit register supplies two adjacent pixels. */
    DlCmd(RDP_SET_FILL_COLOR << 24, (px << 16) | px);
}

/* FILL_RECTANGLE's corners are inclusive; the API here is exclusive on the
 * bottom-right, matching every other rectangle in this codebase. */
void RDP_FillRectangle(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    u32 w0 = (RDP_FILL_RECT << 24) | (ToFx102(x1 - 1) << 12) | ToFx102(y1 - 1);
    u32 w1 = (ToFx102(x0) << 12) | ToFx102(y0);
    DlCmd(w0, w1);
}

/* -----------------------------------------------------------------------
 * Cache maintenance for texture sources
 * --------------------------------------------------------------------- */
void RDP_WritebackSource(const void *addr, int len)
{
    uintptr_t a = (uintptr_t)addr;
    if (a & 0x20000000u)   /* KSEG1: already uncached, nothing to flush */
        return;
    if (len <= 0)
        return;

    uintptr_t start = a & ~(uintptr_t)15;
    uintptr_t end   = (a + (uintptr_t)len + 15) & ~(uintptr_t)15;
    for (uintptr_t p = start; p < end; p += 16)
        asm volatile ("cache 0x19, 0(%0)" :: "r"(p) : "memory");  /* Hit_Writeback_D */
}

void RDP_InvalidateDest(const void *addr, int len)
{
    uintptr_t a = (uintptr_t)addr;
    if (a & 0x20000000u)
        return;
    if (len <= 0)
        return;

    uintptr_t start = a & ~(uintptr_t)15;
    uintptr_t end   = (a + (uintptr_t)len + 15) & ~(uintptr_t)15;
    for (uintptr_t p = start; p < end; p += 16)
        asm volatile ("cache 0x11, 0(%0)" :: "r"(p) : "memory");  /* Hit_Invalidate_D */
}

/* -----------------------------------------------------------------------
 * Texturing
 * --------------------------------------------------------------------- */
void RDP_SetTextureImage(u32 addr, u32 fmt, u32 size, int width)
{
    u32 wm1 = ((u32)width - 1u) & 0x3FFu;
    u32 w0 = (RDP_SET_TEX_IMAGE << 24) | (fmt << 21) | (size << 19) | wm1;
    DlCmd(w0, addr & 0x00FFFFFFu);
}

/* Mask/shift/clamp default to wrap (GBI G_TX_WRAP, mask 0) -- nothing here
 * needs anything else. */
void RDP_SetTile(int tile, u32 fmt, u32 size, int line, int tmemAddr, int palette)
{
    u32 lt = (((u32)line & 0x1FFu) << 9) | ((u32)tmemAddr & 0x1FFu);
    u32 w0 = (RDP_SET_TILE << 24) | (fmt << 21) | (size << 19) | lt;
    u32 w1 = (((u32)tile & 7u) << 24) | (((u32)palette & 0xFu) << 20);
    DlCmd(w0, w1);
}

void RDP_SetTileSize(int tile, int s0, int t0, int s1, int t1)
{
    u32 w0 = (RDP_SET_TILE_SIZE << 24) | (ToFx102(s0) << 12) | ToFx102(t0);
    u32 w1 = (((u32)tile & 7u) << 24) | (ToFx102(s1 - 1) << 12) | ToFx102(t1 - 1);
    DlCmd(w0, w1);
}

void RDP_LoadTile(int tile, int s0, int t0, int s1, int t1)
{
    u32 w0 = (RDP_LOAD_TILE << 24) | (ToFx102(s0) << 12) | ToFx102(t0);
    u32 w1 = (((u32)tile & 7u) << 24) | (ToFx102(s1 - 1) << 12) | ToFx102(t1 - 1);
    DlCmd(w0, w1);
}

void RDP_LoadTlut(int tile, int first, int count)
{
    u32 w0 = (RDP_LOAD_TLUT << 24) | (((u32)first * 4u) << 12);
    u32 w1 = (((u32)tile & 7u) << 24) | ((((u32)first + (u32)count - 1u) * 4u) << 12);
    DlCmd(w0, w1);
}

/* TEXTURE_RECTANGLE is four words: the rectangle, then the texture
 * coordinate at its top-left plus the per-pixel S/T steps (s5.10).
 *
 * The bottom-right corner is the last pixel in COPY/FILL cycle but one
 * past it in 1-/2-cycle -- using the COPY form under 1-cycle mode drops
 * the rectangle's right column and bottom row.
 *
 * sHalf/tHalf are in HALF-texel units (a whole texel is 2), not whole
 * texels -- the extra precision is what lets RDP_TextureRectangle below
 * express the half-texel offset a bilinear sample needs. */
void RDP_TextureRectangleXF(int tile, int x0, int y0, int x1, int y1,
                             int sHalf, int tHalf, int dsdx, int dtdy)
{
    if (x1 <= x0 || y1 <= y0)
        return;

    int xl = x1, yl = y1;
    if (sCycle == CYCLE_COPY) { xl = x1 - 1; yl = y1 - 1; }

    u32 w0 = (RDP_TEXTURE_RECT << 24) | (ToFx102(xl) << 12) | ToFx102(yl);
    u32 w1 = (((u32)tile & 7u) << 24) | (ToFx102(x0) << 12) | ToFx102(y0);
    DlCmd(w0, w1);

    u32 w2 = (((u32)(sHalf * 16) & 0xFFFFu) << 16) | ((u32)(tHalf * 16) & 0xFFFFu);
    u32 w3 = (((u32)dsdx & 0xFFFFu) << 16) | ((u32)dtdy & 0xFFFFu);
    DlCmd(w2, w3);
}

/* The unflipped 1:1 case: one texel per pixel in each axis. COPY retires
 * four pixels per RDP cycle, so its S step has to be four texels' worth
 * (4096 in s5.10) where 1-cycle advances one texel per pixel (1024).
 *
 * bi_lerp0/bi_lerp1 being forced on (see RDP_SetModeStandard) means the
 * texture unit samples bilinearly whenever it is set, and a bilinear
 * sample at an integer texel coordinate lands exactly on the boundary
 * between that texel and its neighbour, blending the two 50/50 instead of
 * reading the one that was asked for. Centring the sample -- offsetting
 * by half a texel -- is what a filtered read always needs; point-sampled
 * reads (FILL, COPY) must not get it, or they end up a half-texel off
 * the other way. A real, separate bug from the background renderer's own
 * still-open one (see the comment above RenderTextLayerRDP in
 * tile_renderer.c) -- fixing this did not fix that. */
void RDP_TextureRectangle(int tile, int x0, int y0, int x1, int y1, int s, int t)
{
    int dsdx = (sCycle == CYCLE_COPY) ? 4096 : 1024;
    int half = sBilerp ? 1 : 0;
    RDP_TextureRectangleXF(tile, x0, y0, x1, y1, s * 2 + half, t * 2 + half, dsdx, 1024);
}
