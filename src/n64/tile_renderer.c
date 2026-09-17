/*
 * src/n64/tile_renderer.c
 *
 * N64 port — software 2D tile/background compositor
 *
 * Replaces the GBA hardware tile engine with a software renderer that:
 *   1. Reads the same tilemap / charblock data the game already builds in
 *      the software VRAM buffer (gN64IoRegs, sw_vram, sw_palette).
 *   2. Composites 4 BG layers in priority order.
 *   3. Handles text-mode BGs (modes 0/1), affine BGs (modes 1/2), and
 *      bitmap mode (mode 3).
 *   4. Applies scroll offsets, tile flipping, 4bpp/8bpp colour depth.
 *   5. Supports affine transformation (PA/PB/PC/PD + reference point).
 *   6. Applies window clipping (WIN0/WIN1/WINOUT/WININ).
 *   7. Applies colour blending (BLDCNT / BLDALPHA / BLDY).
 *   8. Runs the scanline effect array (per-line scroll changes).
 *   9. Writes the final RGBA5551 pixel stream into the VI back buffer.
 *
 * Performance. This is the most expensive thing the port does -- roughly
 * three fifths of a frame -- so the shape of it is deliberate:
 *
 *   - Layers go straight into the VI back buffer, back to front, rather
 *     than into per-layer line buffers that a second pass reads back.
 *   - Whole unflipped tiles of an overlaying layer go out as four pixel
 *     pairs through a per-palette-bank table, with a class byte saying
 *     whether a pair can be stored whole, skipped, or has to go one pixel
 *     at a time. Blank tiles, which are most of what the upper layers
 *     hold, cost one test.
 *   - Flipped tiles get their own unrolled case. They are about a quarter
 *     of what a scene draws, and running them through the ragged per-pixel
 *     path cost more than every unflipped tile put together.
 *   - The tilemap is walked with a pointer instead of deriving each
 *     entry's address from x, and the leading partial tile is the only one
 *     that needs any offset arithmetic.
 *
 * What is left is about five instructions per pixel per layer. Going
 * materially below that means not drawing four layers in software at all,
 * which is what the RDP is for.
 */

#include <string.h>
#include "global.h"
#include "n64/defines.h"
#include "scanline_effect.h"

/* -----------------------------------------------------------------------
 * External framebuffer (declared in vi.c)
 * --------------------------------------------------------------------- */
/* The picture's top-left pixel inside the VI back buffer, and the distance
 * between its rows. The compositor writes there directly; see vi.c. */
extern u16 *gN64FrameBuf;
#define FB_STRIDE N64_VI_WIDTH

/* -----------------------------------------------------------------------
 * Palette helpers
 *
 * GBA palette format: RGB555 (bits 14-10 = B, bits 9-5 = G, bits 4-0 = R)
 * N64 framebuffer: RGBA5551 (bits 15-11 = R, bits 10-6 = G, bits 5-1 = B,
 *                             bit 0 = alpha)
 *
 * Conversion: swap R and B channels, set alpha bit to 1.
 * --------------------------------------------------------------------- */
static inline u16 RGB555toRGBA5551(u16 gba)
{
    u16 r = (gba >>  0) & 0x1F;
    u16 g = (gba >>  5) & 0x1F;
    u16 b = (gba >> 10) & 0x1F;
    /* CPU and VI framebuffer DMA are both big-endian (-EB); no byte-swap. */
    return (u16)((r << 11) | (g << 6) | (b << 1) | 1);
}

/* Palette entries are stored native (LoadPalette byte-swaps the
 * little-endian GBA asset once on the way in), so they read back
 * directly as RGB555 -- the same values the game's own fade/blend code
 * operates on. Tilemap entries and bitmap-mode pixels below are *not*
 * native: nothing but this renderer ever reads them, so they stay as
 * the raw little-endian bytes the assets ship with and get swapped at
 * the point of use. */
static inline u16 PlttRead(const u16 *p, int i)
{
    return p[i];
}

/* -----------------------------------------------------------------------
 * Software memory buffer pointers
 * --------------------------------------------------------------------- */
static inline u16 *PlttBuf(void)   { return (u16 *)__n64_pltt_buf; }
static inline u8  *VramBuf(void)   { return (u8  *)__n64_vram_buf; }
static inline u8  *OamBuf(void)    { return (u8  *)__n64_oam_buf;  }

/* -----------------------------------------------------------------------
 * BG layer descriptor — built each frame from the software IO registers
 * --------------------------------------------------------------------- */
typedef struct {
    int     enabled;
    int     priority;    /* 0 = front, 3 = back */
    int     charBase;    /* charblock base (0-3) → byte offset in VRAM   */
    int     screenBase;  /* screenblock base (0-31) → byte offset         */
    int     bpp8;        /* 0 = 4bpp, 1 = 8bpp                           */
    int     mosaic;
    int     affine;      /* this BG uses affine transform                 */
    int     areaOverflow;/* affine area overflow wrap                     */
    int     screenSize;  /* 0-3: text 256/512 wide/tall, affine 128-1024 */
    int     hOfs;
    int     vOfs;
    s32     pa, pb, pc, pd;  /* affine matrix (8.8 fixed-point)          */
    s32     refX, refY;      /* affine reference point (8.8 fixed-point)  */
} BgDesc;

/* -----------------------------------------------------------------------
 * Parse BG layer descriptors from software IO registers
 * --------------------------------------------------------------------- */
static void ParseBgDesc(int bgNum, BgDesc *bg, int mode)
{
    static const int bgCntOffsets[4] = {
        REG_OFFSET_BG0CNT, REG_OFFSET_BG1CNT,
        REG_OFFSET_BG2CNT, REG_OFFSET_BG3CNT
    };
    static const int bgHOfsOffsets[4] = {
        REG_OFFSET_BG0HOFS, REG_OFFSET_BG1HOFS,
        REG_OFFSET_BG2HOFS, REG_OFFSET_BG3HOFS
    };
    static const int bgVOfsOffsets[4] = {
        REG_OFFSET_BG0VOFS, REG_OFFSET_BG1VOFS,
        REG_OFFSET_BG2VOFS, REG_OFFSET_BG3VOFS
    };

    u16 dispcnt = _REG16(REG_OFFSET_DISPCNT);
    u16 cnt     = _REG16(bgCntOffsets[bgNum]);

    bg->enabled    = (dispcnt >> (8 + bgNum)) & 1;
    bg->priority   = cnt & 3;
    bg->charBase   = ((cnt >> 2) & 3) * BG_CHAR_SIZE;
    bg->screenBase = ((cnt >> 8) & 0x1F) * BG_SCREEN_SIZE;
    bg->bpp8       = (cnt >> 7) & 1;
    bg->mosaic     = (cnt >> 6) & 1;
    bg->screenSize = (cnt >> 14) & 3;
    bg->areaOverflow = (cnt >> 13) & 1;

    bg->affine = (mode == 1 && bgNum == 2) ||
                 (mode == 2 && (bgNum == 2 || bgNum == 3));

    bg->hOfs = _REG16(bgHOfsOffsets[bgNum]) & 0x1FF;
    bg->vOfs = _REG16(bgVOfsOffsets[bgNum]) & 0x1FF;

    if (bg->affine) {
        static const int paOffsets[2] = {REG_OFFSET_BG2PA, REG_OFFSET_BG3PA};
        static const int refXOffsets[2] = {REG_OFFSET_BG2X, REG_OFFSET_BG3X};
        int ai = bgNum - 2;
        bg->pa   = (s32)(s16)_REG16(paOffsets[ai] + 0);
        bg->pb   = (s32)(s16)_REG16(paOffsets[ai] + 2);
        bg->pc   = (s32)(s16)_REG16(paOffsets[ai] + 4);
        bg->pd   = (s32)(s16)_REG16(paOffsets[ai] + 6);
        /* BG2X/BG2Y are 32-bit registers the game writes as two halves
         * (BG2X_L then BG2X_H). The software register file is big-endian,
         * so a plain 32-bit read would put the _L half in the high word --
         * reassemble them explicitly instead. */
        bg->refX = (s32)((_REG16(refXOffsets[ai] + 2) << 16) | _REG16(refXOffsets[ai] + 0));
        bg->refY = (s32)((_REG16(refXOffsets[ai] + 6) << 16) | _REG16(refXOffsets[ai] + 4));
        /* Sign-extend 28-bit value */
        if (bg->refX & 0x08000000) bg->refX |= 0xF0000000;
        if (bg->refY & 0x08000000) bg->refY |= 0xF0000000;
    }
}

/* -----------------------------------------------------------------------
 * Per-scanline layer buffers
 *
 * The compositor used to look each pixel up individually, which meant
 * re-reading the tilemap entry and re-deriving the character address
 * eight times per tile -- around 500 CPU cycles per output pixel once
 * the cache misses were counted, or ~0.2s per frame.  Instead each
 * enabled layer now renders a whole scanline in 8-pixel tile runs, and
 * the compositing pass just walks the resulting line buffers.
 * --------------------------------------------------------------------- */


/* A layer's scanline, already converted to the framebuffer's RGBA5551.
 * RGB555toRGBA5551() always sets the alpha bit, so a zero entry can stand
 * for "transparent" and no separate coverage array is needed. */
static u16 sLine[4][DISPLAY_WIDTH];

/* Palette pre-converted to RGBA5551 once per frame, so the inner loops
 * never convert and never branch on the index:
 *   sPal4 zeroes index 0 of each 16-colour bank (4bpp transparency),
 *   sPal8 zeroes only index 0 (256-colour transparency). */
static u16 sPal4[256];
static u16 sPal8[256];

/* -----------------------------------------------------------------------
 * Pixel-pair table
 *
 * A 4bpp tile row is one word: four bytes, each holding two pixels. For a
 * layer drawn over what is already there -- which is three of the four, and
 * most of the frame -- looking the byte up rather than its two nibbles
 * turns a tile into four loads and four stores instead of eight of each.
 *
 *   sPairs[bank][byte]   the two pixels, ready to store as one word
 *
 * Whether a pair can go out whole is read off the entry itself rather than
 * a second table: every opaque colour carries the RGBA5551 alpha bit and a
 * transparent one is zero, so both pixels are opaque exactly when
 * `p & 0x00010001 == 0x00010001`, and the four pairs of a tile row can be
 * tested together by ANDing them first. A class table alongside this one
 * was another 4 KB of the 8 KB data cache for something two instructions
 * recover.
 *
 * Only the overlay path uses the table: giving the bottom layer its own
 * copy with the backdrop substituted, or even sharing this one, measurably
 * lost -- the extra banks push the working set past the cache for a layer
 * that is a quarter of the work. Banks are built on first use in a frame,
 * so a scene pays for the handful it draws with rather than all sixteen.
 * --------------------------------------------------------------------- */
static u32 sPairs[16][256];
static u16 sPairBuilt;   /* bank bitmask, cleared each frame */

/* Both pixels of a pair opaque. */
#define PAIR_BOTH 0x00010001u

static void BuildPairBank(u32 bank)
{
    const u16 *pal = sPal4 + bank * 16;
    u32 *pairs = sPairs[bank];

    for (int b = 0; b < 256; b++) {
        /* Within a byte the low nibble is the left pixel. */
        u16 p0 = pal[b & 0xF];
        u16 p1 = pal[b >> 4];

        pairs[b] = ((u32)p0 << 16) | p1;
    }

    sPairBuilt |= (u16)(1u << bank);
}

static void BuildPaletteCache(const u16 *pltt)
{
    for (int i = 0; i < 256; i++) {
        u16 c = RGB555toRGBA5551(PlttRead(pltt, i));
        sPal8[i] = c;
        sPal4[i] = (i & 0xF) ? c : 0;
    }
    sPal8[0] = 0;
    sPairBuilt = 0;
}

/* How a layer's pixels reach their destination.
 *
 * The compositor used to render every enabled layer into its own line
 * buffer and then walk all four buffers again to combine them, which meant
 * every background pixel was written once and read back once. In the common
 * case -- no windows, no colour effect -- the layers can go straight into
 * the framebuffer row instead, back to front, and that second pass
 * disappears along with the line buffers it read.
 *
 * The mode is always a literal at the call site and the renderers are
 * always_inline, so each instantiation compiles down to just its own store
 * with no test of `mode` left in the inner loop. */
#define LAYER_WRITE 0   /* store every pixel; transparent becomes 0        */
#define LAYER_BASE  1   /* store every pixel; transparent becomes `fill`   */
#define LAYER_OVER  2   /* store only opaque pixels, leaving the rest      */

/* One pixel pair of an overlaying layer: store it whole when both pixels
 * are opaque, drop it when neither is, otherwise one at a time. */
#define PAIR_OVER(q, o, x, i, pairval)                                      \
    do {                                                                    \
        u32 p__ = (pairval);                                                \
        if ((p__ & PAIR_BOTH) == PAIR_BOTH) { (q)[i] = p__; }               \
        else if (p__) {                                                     \
            if (p__ >> 16)     (o)[(x) + (i) * 2 + 0] = (u16)(p__ >> 16);   \
            if (p__ & 0xFFFFu) (o)[(x) + (i) * 2 + 1] = (u16)p__;           \
        }                                                                   \
    } while (0)

/* The same pair, for the bottom layer: nothing shows through it, so a
 * transparent pixel becomes the backdrop and every pair is one store.
 * `fp` is the backdrop doubled into a word. */
#define PAIR_BASE(q, i, pairval, fp)                                        \
    do {                                                                    \
        u32 p__ = (pairval);                                                \
        if ((p__ & PAIR_BOTH) == PAIR_BOTH) { (q)[i] = p__; }               \
        else if (!p__) { (q)[i] = (fp); }                                   \
        else {                                                              \
            u32 hi__ = p__ >> 16, lo__ = p__ & 0xFFFFu;                     \
            if (!hi__) hi__ = (fp) >> 16;                                   \
            if (!lo__) lo__ = (fp) & 0xFFFFu;                               \
            (q)[i] = (hi__ << 16) | lo__;                                   \
        }                                                                   \
    } while (0)

#define LAYER_PUT(dst, colour)                       \
    do {                                             \
        u16 c_ = (colour);                           \
        if (mode == LAYER_OVER) { if (c_) (dst) = c_; } \
        else if (mode == LAYER_BASE) (dst) = c_ ? c_ : fill; \
        else (dst) = c_;                             \
    } while (0)

/* -----------------------------------------------------------------------
 * One 4bpp tile row, emitted into the framebuffer
 *
 * Shared by the two renderers below: the per-scanline one, and the
 * tile-major one that draws eight rows of a tile before moving on. `o`
 * points at the first pixel, `first` is which pixel of the tile it is, and
 * `n` how many of them there are.
 * --------------------------------------------------------------------- */
static inline __attribute__((always_inline))
void EmitTileRow4(u16 *o, u32 w, const u16 *pal,
                  const u32 *bankPairs,
                  int first, int n, int hFlip, int mode, u16 fill)
{
    if (mode == LAYER_OVER && n == 8 && !hFlip && !(((uintptr_t)o) & 3)) {
        /* The hot case: an unflipped tile of an overlaying layer landing on
         * a word boundary, so it goes out as four pixel pairs. Whether it is
         * word-aligned is fixed for a whole line by the scroll offset, so
         * the test predicts perfectly. */
        if (w == 0) {
            /* A blank tile contributes nothing when overlaying, and blank
             * tiles are most of what the upper layers hold. */
        } else {
            u32 *q = (u32 *)o;
            u32 p0 = bankPairs[(w >> 24) & 0xFF];
            u32 p1 = bankPairs[(w >> 16) & 0xFF];
            u32 p2 = bankPairs[(w >>  8) & 0xFF];
            u32 p3 = bankPairs[w & 0xFF];

            /* A fully opaque tile is the common case and skips the per-pair
             * tests entirely. */
            if (((p0 & p1 & p2 & p3) & PAIR_BOTH) == PAIR_BOTH) {
                q[0] = p0;
                q[1] = p1;
                q[2] = p2;
                q[3] = p3;
            } else {
                PAIR_OVER(q, o, 0, 0, p0);
                PAIR_OVER(q, o, 0, 1, p1);
                PAIR_OVER(q, o, 0, 2, p2);
                PAIR_OVER(q, o, 0, 3, p3);
            }
        }
    } else if (mode == LAYER_BASE && n == 8 && !hFlip && !(((uintptr_t)o) & 3)) {
        /* The bottom layer, through the same table: nothing shows through
         * it, so a transparent pixel is just the backdrop and every pair is
         * one store rather than two. */
        u32 *q = (u32 *)o;
        u32 p0 = bankPairs[(w >> 24) & 0xFF];
        u32 p1 = bankPairs[(w >> 16) & 0xFF];
        u32 p2 = bankPairs[(w >>  8) & 0xFF];
        u32 p3 = bankPairs[w & 0xFF];

        if (((p0 & p1 & p2 & p3) & PAIR_BOTH) == PAIR_BOTH) {
            q[0] = p0;
            q[1] = p1;
            q[2] = p2;
            q[3] = p3;
        } else {
            u32 fp = ((u32)fill << 16) | fill;
            PAIR_BASE(q, 0, p0, fp);
            PAIR_BASE(q, 1, p1, fp);
            PAIR_BASE(q, 2, p2, fp);
            PAIR_BASE(q, 3, p3, fp);
        }
    } else if (mode != LAYER_WRITE && n == 8 && !hFlip) {
        /* The same tile, landing half a pixel out. A scroll offset with an
         * odd low bit puts every tile of the line on an odd halfword, which
         * used to drop the whole line onto the eight-store path below --
         * about a sixth of everything drawn. The pairs can straddle the
         * bytes instead: one pixel on its own at each end, and three pairs
         * in between, each built from the tail of one table entry and the
         * head of the next. */
        u32 P0 = bankPairs[(w >> 24) & 0xFF];
        u32 P1 = bankPairs[(w >> 16) & 0xFF];
        u32 P2 = bankPairs[(w >>  8) & 0xFF];
        u32 P3 = bankPairs[w & 0xFF];

        if (mode == LAYER_OVER && w == 0) {
            /* blank tile, nothing to overlay */
        } else {
            u32 *q = (u32 *)(o + 1);
            u32 c1 = (P0 << 16) | (P1 >> 16);
            u32 c2 = (P1 << 16) | (P2 >> 16);
            u32 c3 = (P2 << 16) | (P3 >> 16);
            u16 lead = (u16)(P0 >> 16);
            u16 tail = (u16)P3;

            if (((P0 & P1 & P2 & P3) & PAIR_BOTH) == PAIR_BOTH) {
                o[0] = lead;
                q[0] = c1;
                q[1] = c2;
                q[2] = c3;
                o[7] = tail;
            } else if (mode == LAYER_BASE) {
                u32 fp = ((u32)fill << 16) | fill;
                o[0] = lead ? lead : fill;
                PAIR_BASE(q, 0, c1, fp);
                PAIR_BASE(q, 1, c2, fp);
                PAIR_BASE(q, 2, c3, fp);
                o[7] = tail ? tail : fill;
            } else {
                if (lead) o[0] = lead;
                PAIR_OVER(q, o, 1, 0, c1);
                PAIR_OVER(q, o, 1, 1, c2);
                PAIR_OVER(q, o, 1, 2, c3);
                if (tail) o[7] = tail;
            }
        }
    } else if (n == 8 && !hFlip) {
        /* A whole unflipped tile with no table to draw it from. */
        if (mode == LAYER_OVER && w == 0) {
            /* blank tile, nothing to overlay */
        } else {
            LAYER_PUT(o[0], pal[(w >> 24) & 0xF]);
            LAYER_PUT(o[1], pal[(w >> 28) & 0xF]);
            LAYER_PUT(o[2], pal[(w >> 16) & 0xF]);
            LAYER_PUT(o[3], pal[(w >> 20) & 0xF]);
            LAYER_PUT(o[4], pal[(w >>  8) & 0xF]);
            LAYER_PUT(o[5], pal[(w >> 12) & 0xF]);
            LAYER_PUT(o[6], pal[(w >>  0) & 0xF]);
            LAYER_PUT(o[7], pal[(w >>  4) & 0xF]);
        }
    } else if (n == 8 && hFlip) {
        /* A whole flipped tile. Flipped tiles are a quarter of everything a
         * scene draws, and sending them through the ragged path below --
         * which works the shift out per pixel -- cost more than all the
         * unflipped ones together. */
        if (mode == LAYER_OVER && w == 0) {
            /* blank tile, nothing to overlay */
        } else {
            LAYER_PUT(o[0], pal[(w >>  4) & 0xF]);
            LAYER_PUT(o[1], pal[(w >>  0) & 0xF]);
            LAYER_PUT(o[2], pal[(w >> 12) & 0xF]);
            LAYER_PUT(o[3], pal[(w >>  8) & 0xF]);
            LAYER_PUT(o[4], pal[(w >> 20) & 0xF]);
            LAYER_PUT(o[5], pal[(w >> 16) & 0xF]);
            LAYER_PUT(o[6], pal[(w >> 28) & 0xF]);
            LAYER_PUT(o[7], pal[(w >> 24) & 0xF]);
        }
    } else {
        /* A partial tile at either end of the line. Few enough of these that
         * the per-pixel shift is not worth unrolling. */
        for (int i = 0; i < n; i++) {
            int sx = first + i;
            if (hFlip) sx = 7 - sx;
            int shift = (sx & 1) * 4 + (3 - (sx >> 1)) * 8;
            LAYER_PUT(o[i], pal[(w >> shift) & 0xF]);
        }
    }
}

/* -----------------------------------------------------------------------
 * Text-mode (modes 0/1) scanline renderer
 * --------------------------------------------------------------------- */
static inline __attribute__((always_inline))
void RenderTextLineImpl(const BgDesc *bg, int y, u16 *out, int mode, u16 fill)
{
    const u8 *vram = VramBuf();

    /* Everything the tile loop needs that does not change across the line,
     * lifted out of it: the loop runs thirty-odd times per layer per
     * scanline, so its setup costs about as much as the pixels do. */
    const int screenSize = bg->screenSize;
    const int bpp8       = bg->bpp8;
    const int charBase   = bg->charBase;
    const int hOfs       = bg->hOfs;

    int mapW = 256 << (screenSize & 1);   /* 256 or 512 px wide      */
    int mapH = 256 << (screenSize >> 1);  /* 256 or 512 px tall      */
    int mapWMask = mapW - 1;

    int ty  = (y + bg->vOfs) & (mapH - 1);
    int sby = ty >> 8;
    int tileY = (ty & 0xFF) >> 3;
    int rowInTile = ty & 7;

    /* Which screenblock a tile falls in splits cleanly into a vertical part
     * fixed for the whole line and a horizontal part that is either zero or
     * the top bit of tx: 512-wide maps put screenblocks 0,1 side by side,
     * 512-tall maps stack them, and 512x512 uses 0,1 over 2,3. */
    int sbyPart  = (screenSize == 2) ? sby : (screenSize == 3) ? sby * 2 : 0;
    int splitX   = screenSize & 1;
    const u8 *mapRow = vram + bg->screenBase + sbyPart * BG_SCREEN_SIZE
                     + tileY * 64;

    /* Walk the tilemap entries rather than deriving each address from x:
     * they are two bytes apart until the row wraps at 32 tiles, where a
     * 512-wide map crosses into its second screenblock. Only the first tile
     * of a line can start partway in, so `first` is zero from the second
     * iteration onward and the whole-tile case needs no arithmetic. */
    int tx = hOfs & mapWMask;
    int tileX = (tx & 0xFF) >> 3;
    int block = splitX ? (tx >> 8) : 0;
    const u8 *entryPtr = mapRow + block * BG_SCREEN_SIZE + tileX * 2;
    int first = tx & 7;

    /* Neighbouring tiles nearly always share a palette bank, so its tables
     * are looked up once and carried along the line. */
    int lastBank = -1;
    const u32 *bankPairs = NULL;

    int x = 0;
    while (x < DISPLAY_WIDTH)
    {
        u16 entry = __builtin_bswap16(*(const u16 *)entryPtr);

        int tileNum = entry & 0x3FF;
        int hFlip   = (entry >> 10) & 1;
        int vFlip   = (entry >> 11) & 1;
        int py      = vFlip ? 7 - rowInTile : rowInTile;

        int n = 8 - first;                  /* pixels left in the tile    */
        if (x + n > DISPLAY_WIDTH)
            n = DISPLAY_WIDTH - x;

        if (bpp8)
        {
            const u8 *row = vram + charBase + tileNum * TILE_SIZE_8BPP + py * 8;
            for (int i = 0; i < n; i++) {
                int sx = first + i;
                if (hFlip) sx = 7 - sx;
                LAYER_PUT(out[x + i], sPal8[row[sx]]);
            }
        }
        else
        {
            /* Tile rows are 4 bytes and always 4-byte aligned, so the whole
             * row comes in with one load. Big-endian: byte 0 is the top
             * byte, and within a byte the low nibble is the left pixel. */
            u32 w = *(const u32 *)(vram + charBase
                                   + tileNum * TILE_SIZE_4BPP + py * 4);
            int bank = (entry >> 12) & 0xF;

            if (mode != LAYER_WRITE && bank != lastBank) {
                lastBank = bank;
                if (!(sPairBuilt & (1u << bank)))
                    BuildPairBank(bank);
                bankPairs = sPairs[bank];
            }

            EmitTileRow4(out + x, w, sPal4 + bank * 16, bankPairs,
                         first, n, hFlip, mode, fill);
        }

        x += n;
        first = 0;

        if (++tileX == 32) {
            tileX = 0;
            block ^= splitX;
            entryPtr = mapRow + block * BG_SCREEN_SIZE;
        } else {
            entryPtr += 2;
        }
    }
}


/* -----------------------------------------------------------------------
 * Text-mode renderer, tile-major
 *
 * The per-scanline renderer above re-reads a tile's map entry and one of
 * its rows for every line it appears on: eight touches of the same 32-byte
 * tile, scattered across a 16 KB charblock. On a 93.75 MHz CPU with an 8 KB
 * data cache that is where the frame goes -- measured against ares, which
 * models the cache, about two fifths of the background pass was memory
 * stalls rather than instructions.
 *
 * This draws a band of up to eight scanlines at once, finishing each tile
 * before moving to the next, so a tile's bytes are read once and used
 * eight times. The writes become strided instead of sequential, but they
 * touch the same number of framebuffer lines either way.
 *
 * It cannot be used when a scanline effect is running, since that rewrites
 * the scroll registers between lines; the caller checks.
 * --------------------------------------------------------------------- */
static inline __attribute__((always_inline))
void RenderTextBandImpl(const BgDesc *bg, int y0, int rows, int mode, u16 fill)
{
    const u8 *vram       = VramBuf();
    const int screenSize = bg->screenSize;
    const int charBase   = bg->charBase;
    const int hOfs       = bg->hOfs;

    int mapW     = 256 << (screenSize & 1);
    int mapH     = 256 << (screenSize >> 1);
    int mapWMask = mapW - 1;
    int splitX   = screenSize & 1;

    int r = 0;
    while (r < rows)
    {
        /* How many of the band's remaining rows come from one tile row. */
        int ty        = (y0 + r + bg->vOfs) & (mapH - 1);
        int rowInTile = ty & 7;
        int n         = 8 - rowInTile;
        if (n > rows - r)
            n = rows - r;

        int sby     = ty >> 8;
        int tileY   = (ty & 0xFF) >> 3;
        int sbyPart = (screenSize == 2) ? sby : (screenSize == 3) ? sby * 2 : 0;
        const u8 *mapRow = vram + bg->screenBase + sbyPart * BG_SCREEN_SIZE
                         + tileY * 64;

        int tx    = hOfs & mapWMask;
        int tileX = (tx & 0xFF) >> 3;
        int block = splitX ? (tx >> 8) : 0;
        const u8 *entryPtr = mapRow + block * BG_SCREEN_SIZE + tileX * 2;
        int first = tx & 7;

        int lastBank = -1;
        const u32 *bankPairs = NULL;

        u16 *rowBase = gN64FrameBuf + (y0 + r) * FB_STRIDE;

        int x = 0;
        while (x < DISPLAY_WIDTH)
        {
            u16 entry = __builtin_bswap16(*(const u16 *)entryPtr);

            int tileNum = entry & 0x3FF;
            int hFlip   = (entry >> 10) & 1;
            int vFlip   = (entry >> 11) & 1;
            int bank    = (entry >> 12) & 0xF;

            int cols = 8 - first;
            if (x + cols > DISPLAY_WIDTH)
                cols = DISPLAY_WIDTH - x;

            if (mode != LAYER_WRITE && bank != lastBank) {
                lastBank = bank;
                if (!(sPairBuilt & (1u << bank)))
                    BuildPairBank(bank);
                bankPairs = sPairs[bank];
            }

            const u16 *pal      = sPal4 + bank * 16;
            const u8  *tileBase = vram + charBase + tileNum * TILE_SIZE_4BPP;

            /* A tile of an overlaying layer that is blank everywhere
             * contributes nothing at all, and blank tiles are most of what
             * the upper layers hold. Four 64-bit loads answer that for the
             * whole tile -- MIPS III has them and a tile is 32-byte aligned
             * -- where the row loop would ask eight times. The loads warm
             * the same cache lines the rows below read, so the check costs
             * nothing when the tile is not blank. */
            if (mode == LAYER_OVER && n == 8) {
                const u64 *q = (const u64 *)tileBase;
                if ((q[0] | q[1] | q[2] | q[3]) == 0) {
                    x += cols;
                    first = 0;
                    if (++tileX == 32) {
                        tileX = 0;
                        block ^= splitX;
                        entryPtr = mapRow + block * BG_SCREEN_SIZE;
                    } else {
                        entryPtr += 2;
                    }
                    continue;
                }
            }

            /* Every row of this tile that the band needs, while its bytes
             * are in the cache. */
            for (int i = 0; i < n; i++) {
                int py = vFlip ? 7 - (rowInTile + i) : (rowInTile + i);
                u32 w = *(const u32 *)(tileBase + py * 4);
                EmitTileRow4(rowBase + i * FB_STRIDE + x, w, pal,
                             bankPairs, first, cols, hFlip, mode, fill);
            }

            x += cols;
            first = 0;

            if (++tileX == 32) {
                tileX = 0;
                block ^= splitX;
                entryPtr = mapRow + block * BG_SCREEN_SIZE;
            } else {
                entryPtr += 2;
            }
        }

        r += n;
    }
}

static void RenderTextBandBase(const BgDesc *bg, int y0, int rows, u16 fill)
{
    RenderTextBandImpl(bg, y0, rows, LAYER_BASE, fill);
}

static void RenderTextBandOver(const BgDesc *bg, int y0, int rows)
{
    RenderTextBandImpl(bg, y0, rows, LAYER_OVER, 0);
}

/* -----------------------------------------------------------------------
 * Affine (modes 1/2) scanline renderer
 *
 * Steps the texture coordinate by pa/pc across the line instead of
 * recomputing the full matrix product per pixel, and masks instead of
 * dividing -- affine map sizes are always powers of two.
 * --------------------------------------------------------------------- */
static inline __attribute__((always_inline))
void RenderAffineLineImpl(const BgDesc *bg, int y, u16 *out, int mode, u16 fill)
{
    const u8 *vram = VramBuf();

    static const int affineMapSizes[4] = {128, 256, 512, 1024};
    int mapSize  = affineMapSizes[bg->screenSize];
    int mapMask  = mapSize - 1;
    int mapTiles = mapSize >> 3;

    s32 texX = bg->refX + bg->pb * y;
    s32 texY = bg->refY + bg->pd * y;

    for (int x = 0; x < DISPLAY_WIDTH; x++, texX += bg->pa, texY += bg->pc)
    {
        int px = texX >> 8;
        int py = texY >> 8;

        if (bg->areaOverflow) {
            px &= mapMask;
            py &= mapMask;
        } else if (px < 0 || px >= mapSize || py < 0 || py >= mapSize) {
            LAYER_PUT(out[x], 0);
            continue;
        }

        /* Affine screenblocks hold 1-byte entries and are always 8bpp */
        u8 tileNum = vram[bg->screenBase + (py >> 3) * mapTiles + (px >> 3)];
        LAYER_PUT(out[x], sPal8[vram[bg->charBase + tileNum * TILE_SIZE_8BPP
                                     + (py & 7) * 8 + (px & 7)]]);
    }
}

/* -----------------------------------------------------------------------
 * One layer's scanline, in whichever of the three destination modes the
 * caller needs. `bgMode` picks the renderer; anything a mode does not
 * define contributes nothing.
 * --------------------------------------------------------------------- */
static inline __attribute__((always_inline))
void RenderBgLineImpl(const BgDesc *bg, int bgIdx, int bgMode, int y,
                      u16 *out, int mode, u16 fill)
{
    if (bgMode == 0 || (bgMode == 1 && bgIdx < 2)) {
        RenderTextLineImpl(bg, y, out, mode, fill);
    } else if ((bgMode == 1 && bgIdx == 2) || bgMode == 2) {
        RenderAffineLineImpl(bg, y, out, mode, fill);
    } else if (bgMode == 3 && bgIdx == 2) {
        /* Bitmap mode 3: 240x160 direct-colour (LE bytes from ROM) */
        const u16 *src = (const u16 *)(VramBuf() + y * DISPLAY_WIDTH * 2);
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            LAYER_PUT(out[x], RGB555toRGBA5551(__builtin_bswap16(src[x])));
    } else if (bgMode == 4 && bgIdx == 2) {
        /* Bitmap mode 4: 240x160 8bpp paletted */
        int frame = (_REG16(REG_OFFSET_DISPCNT) >> 4) & 1;
        const u8 *src = VramBuf() + frame * 0xA000 + y * DISPLAY_WIDTH;
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            LAYER_PUT(out[x], sPal8[src[x]]);
    } else if (mode != LAYER_OVER) {
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            LAYER_PUT(out[x], 0);
    }
}

static void RenderBgLineWrite(const BgDesc *bg, int bgIdx, int bgMode, int y, u16 *out)
{
    RenderBgLineImpl(bg, bgIdx, bgMode, y, out, LAYER_WRITE, 0);
}

static void RenderBgLineBase(const BgDesc *bg, int bgIdx, int bgMode, int y,
                             u16 *out, u16 fill)
{
    RenderBgLineImpl(bg, bgIdx, bgMode, y, out, LAYER_BASE, fill);
}

static void RenderBgLineOver(const BgDesc *bg, int bgIdx, int bgMode, int y, u16 *out)
{
    RenderBgLineImpl(bg, bgIdx, bgMode, y, out, LAYER_OVER, 0);
}

/* RGBA5551 back to the GBA's RGB555, for the blending paths which work in
 * the palette's own colour space. */
static inline u16 RGBA5551toRGB555(u16 c)
{
    return (u16)(((c >> 11) & 0x1F) | (((c >> 6) & 0x1F) << 5) | (((c >> 1) & 0x1F) << 10));
}

/* -----------------------------------------------------------------------
 * Window mask for a pixel
 *
 * Returns the layer enable bits for a given pixel position.
 * Bit layout mirrors GBA WININ/WINOUT: bits 5-0 = BG0-BG3, OBJ, effects.
 * --------------------------------------------------------------------- */
static u8 sWinMaskRow[DISPLAY_WIDTH];

/* Fills sWinMaskRow for one scanline. Returns 0 if no window is enabled,
 * in which case the row is left untouched and every pixel is 0x3F. */
static int BuildWindowMaskRow(int y)
{
    u16 dispcnt = _REG16(REG_OFFSET_DISPCNT);
    int win0en  = (dispcnt >> 13) & 1;
    int win1en  = (dispcnt >> 14) & 1;

    if (!win0en && !win1en)
        return 0;  /* all layers visible, no windowing */

    u16 winin  = _REG16(REG_OFFSET_WININ);
    u16 winout = _REG16(REG_OFFSET_WINOUT);

    u8 outMask = (u8)(winout & 0x3F);
    u8 in0Mask = (u8)(winin & 0x3F);
    u8 in1Mask = (u8)((winin >> 8) & 0x3F);

    int x0a = 0, x0b = 0, x1a = 0, x1b = 0;

    if (win0en) {
        u16 win0h = _REG16(REG_OFFSET_WIN0H);
        u16 win0v = _REG16(REG_OFFSET_WIN0V);
        int y1 = (win0v >> 8) & 0xFF, y2 = win0v & 0xFF;
        if (y >= y1 && y < y2) {
            x0a = (win0h >> 8) & 0xFF;
            x0b = win0h & 0xFF;
        }
    }
    if (win1en) {
        u16 win1h = _REG16(REG_OFFSET_WIN1H);
        u16 win1v = _REG16(REG_OFFSET_WIN1V);
        int y1 = (win1v >> 8) & 0xFF, y2 = win1v & 0xFF;
        if (y >= y1 && y < y2) {
            x1a = (win1h >> 8) & 0xFF;
            x1b = win1h & 0xFF;
        }
    }

    for (int x = 0; x < DISPLAY_WIDTH; x++) {
        if (x >= x0a && x < x0b)
            sWinMaskRow[x] = in0Mask;
        else if (x >= x1a && x < x1b)
            sWinMaskRow[x] = in1Mask;
        else
            sWinMaskRow[x] = outMask;
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * Colour blend / brightness adjustment
 *
 * All three effects are per-channel scalings of a 5-bit value, so a 32-entry
 * table per coefficient answers them with one load instead of a multiply,
 * a divide and a clamp. The coefficients live in registers a scanline
 * effect is allowed to rewrite mid-frame -- Emerald's fades do exactly that
 * through BLDY -- so the tables are rebuilt whenever the registers move,
 * and only then.
 * --------------------------------------------------------------------- */
static u8 sEvaTab[32], sEvbTab[32], sBriUpTab[32], sBriDownTab[32];
static int sEffCachedAlpha = -1, sEffCachedY = -1;

static void RefreshEffectTables(void)
{
    u16 bldalpha = _REG16(REG_OFFSET_BLDALPHA);
    u16 bldy     = _REG16(REG_OFFSET_BLDY) & 0x1F;

    if ((int)bldalpha != sEffCachedAlpha) {
        int eva = bldalpha & 0x1F;
        int evb = (bldalpha >> 8) & 0x1F;
        if (eva > 16) eva = 16;
        if (evb > 16) evb = 16;
        for (int v = 0; v < 32; v++) {
            sEvaTab[v] = (u8)(v * eva / 16);
            sEvbTab[v] = (u8)(v * evb / 16);
        }
        sEffCachedAlpha = bldalpha;
    }

    if ((int)bldy != sEffCachedY) {
        int ey = bldy > 16 ? 16 : bldy;
        for (int v = 0; v < 32; v++) {
            int up = v + (31 - v) * ey / 16;
            int dn = v - v * ey / 16;
            sBriUpTab[v]   = (u8)(up > 31 ? 31 : up);
            sBriDownTab[v] = (u8)(dn < 0 ? 0 : dn);
        }
        sEffCachedY = bldy;
    }
}

static inline u16 BlendColours(u16 top, u16 bot)
{
    /* GBA RGB555 format */
    int r = sEvaTab[top & 0x1F]         + sEvbTab[bot & 0x1F];
    int g = sEvaTab[(top >> 5) & 0x1F]  + sEvbTab[(bot >> 5) & 0x1F];
    int b = sEvaTab[(top >> 10) & 0x1F] + sEvbTab[(bot >> 10) & 0x1F];
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (u16)(r | (g << 5) | (b << 10));
}

static inline u16 BrightnessIncrease(u16 colour)
{
    return (u16)(sBriUpTab[colour & 0x1F]
               | (sBriUpTab[(colour >> 5) & 0x1F] << 5)
               | (sBriUpTab[(colour >> 10) & 0x1F] << 10));
}

static inline u16 BrightnessDecrease(u16 colour)
{
    return (u16)(sBriDownTab[colour & 0x1F]
               | (sBriDownTab[(colour >> 5) & 0x1F] << 5)
               | (sBriDownTab[(colour >> 10) & 0x1F] << 10));
}

/* -----------------------------------------------------------------------
 * Per-scanline scroll effect support
 *
 * The game uses HBlank DMA to change BG scroll registers per scanline
 * (e.g. battle wave effects).  On GBA this is done in hardware; on N64
 * we snapshot the registers before rendering each line.
 *
 * The game's scanline_effect.c builds gScanlineEffectRegBuffers[][], which
 * contains per-line register values.  We call its update hook before each
 * scanline.
 * --------------------------------------------------------------------- */
extern void ScanlineEffect_ApplyLine(int line);

/* -----------------------------------------------------------------------
 * N64_CompositeFrame — main entry point, called each VBlank
 *
 * Renders one 240×160 frame into gN64GBAFramebuffer.
 * --------------------------------------------------------------------- */
void N64_CompositeFrame(void)
{
    u16 dispcnt = _REG16(REG_OFFSET_DISPCNT);
    int bgMode  = dispcnt & 0x7;

    /* Forced blank: fill with white */
    if (dispcnt & DISPCNT_FORCED_BLANK) {
        for (int y = 0; y < DISPLAY_HEIGHT; y++)
            memset(gN64FrameBuf + y * FB_STRIDE, 0xFF,
                   DISPLAY_WIDTH * sizeof(u16));
        return;
    }

    /* Parse BG descriptors for this frame */
    BgDesc bgs[4];
    for (int i = 0; i < 4; i++)
        ParseBgDesc(i, &bgs[i], bgMode);

    /* Sort BG layer indices by priority (stable, highest priority first) */
    int layerOrder[4] = {0, 1, 2, 3};
    /* Insertion sort by priority */
    for (int i = 1; i < 4; i++) {
        int key = layerOrder[i];
        int j = i - 1;
        while (j >= 0 && bgs[layerOrder[j]].priority > bgs[key].priority) {
            layerOrder[j + 1] = layerOrder[j];
            j--;
        }
        layerOrder[j + 1] = key;
    }

    u16 bldcnt  = _REG16(REG_OFFSET_BLDCNT);
    int blendEff = (bldcnt >> 6) & 3;
    u16 tgt1Mask = bldcnt & 0x3F;
    u16 tgt2Mask = (bldcnt >> 8) & 0x3F;
    u16 *pltt    = PlttBuf();

    BuildPaletteCache(pltt);

    /* Backdrop colour (BG palette entry 0) */
    u16 backdropRGB555 = PlttRead(pltt, 0);
    u16 backdropRGBA   = RGB555toRGBA5551(backdropRGB555);

    /* Fast path: no windows, and no colour effect that can reach anything.
     * A non-zero BLDCNT effect with an empty target-1 mask cannot change a
     * single pixel, and the intro spends its whole run in exactly that
     * state, so testing the mask rather than just the effect keeps those
     * frames out of the expensive path. */
    int fastPath = (blendEff == 0 || tgt1Mask == 0)
                && !((dispcnt >> 13) & 1) && !((dispcnt >> 14) & 1);

    /* Back-to-front order of the enabled layers, for the fast path. */
    int drawOrder[4], drawCount = 0;
    for (int li = 3; li >= 0; li--) {
        int bgIdx = layerOrder[li];
        if (bgs[bgIdx].enabled)
            drawOrder[drawCount++] = bgIdx;
    }

    /* Tile-major needs the scroll registers to hold still for eight lines
     * at a time and every layer to be a 4bpp text layer. That covers all but
     * a few per cent of frames; the rest fall back to per-scanline. */
    extern struct ScanlineEffect gScanlineEffect;
    int bandable = fastPath && drawCount > 0 && gScanlineEffect.state == 0;
    for (int d = 0; d < drawCount && bandable; d++) {
        int bgIdx = drawOrder[d];
        if (bgs[bgIdx].bpp8)
            bandable = 0;
        else if (!(bgMode == 0 || (bgMode == 1 && bgIdx < 2)))
            bandable = 0;
    }

    if (bandable) {
        for (int y = 0; y < DISPLAY_HEIGHT; y += 8) {
            int rows = DISPLAY_HEIGHT - y;
            if (rows > 8)
                rows = 8;

            int bgIdx = drawOrder[0];
            RenderTextBandBase(&bgs[bgIdx], y, rows, backdropRGBA);
            for (int d = 1; d < drawCount; d++)
                RenderTextBandOver(&bgs[drawOrder[d]], y, rows);
        }
        return;
    }

    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        /* Apply per-scanline register changes (battle wave effects, etc.) */
        ScanlineEffect_ApplyLine(y);

        /* Re-read scroll registers for this scanline */
        for (int i = 0; i < 4; i++)
            ParseBgDesc(i, &bgs[i], bgMode);

        u16 *rowOut = gN64FrameBuf + y * FB_STRIDE;

        if (fastPath) {
            /* Straight into the framebuffer row, back to front. The
             * bottommost layer substitutes the backdrop for its own
             * transparent pixels, so the row needs no separate clear. */
            if (drawCount == 0) {
                for (int x = 0; x < DISPLAY_WIDTH; x++)
                    rowOut[x] = backdropRGBA;
            } else {
                int bgIdx = drawOrder[0];
                RenderBgLineBase(&bgs[bgIdx], bgIdx, bgMode, y, rowOut, backdropRGBA);
                for (int d = 1; d < drawCount; d++) {
                    bgIdx = drawOrder[d];
                    RenderBgLineOver(&bgs[bgIdx], bgIdx, bgMode, y, rowOut);
                }
            }
            continue;
        }

        int windowed = BuildWindowMaskRow(y);
        RefreshEffectTables();

        /* Render each enabled layer's scanline into its own line buffer */
        for (int bgIdx = 0; bgIdx < 4; bgIdx++) {
            if (bgs[bgIdx].enabled)
                RenderBgLineWrite(&bgs[bgIdx], bgIdx, bgMode, y, sLine[bgIdx]);
        }

        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            u16 winMask = windowed ? sWinMaskRow[x] : 0x3F;

            /* Composite layers from front to back */
            u16 topColour  = backdropRGB555;  /* fallback = backdrop       */
            u16 botColour  = backdropRGB555;
            int topLayer   = -1;              /* -1 = backdrop              */
            int botLayer   = -1;
            int gotTop     = 0;
            int gotBot     = 0;

            for (int li = 0; li < 4 && !gotBot; li++) {
                int bgIdx = layerOrder[li];

                if (!bgs[bgIdx].enabled) continue;
                if (!(winMask & (1 << bgIdx))) continue;

                u16 v = sLine[bgIdx][x];
                if (!v) continue;

                u16 colour = RGBA5551toRGB555(v);
                if (!gotTop) {
                    topColour = colour;
                    topLayer  = bgIdx;
                    gotTop    = 1;
                } else {
                    botColour = colour;
                    botLayer  = bgIdx;
                    gotBot    = 1;
                }
            }

            /* Apply colour effects. Bit 5 of the window mask is the
             * colour-special-effect enable: a window can exempt what it
             * covers from the blend or brightness pass. The main menu
             * relies on that to darken everything except the highlighted
             * entry, which without this came out uniformly grey. */
            u16 finalColour = topColour;
            if (!(winMask & 0x20))
            {
                /* effect disabled here */
            } else if (blendEff == 1 && gotTop &&
                (tgt1Mask & (topLayer < 0 ? 0x20 : (1 << topLayer))) &&
                (tgt2Mask & (botLayer < 0 ? 0x20 : (1 << botLayer))))
            {
                finalColour = BlendColours(topColour, botColour);
            } else if (blendEff == 2 &&
                (tgt1Mask & (topLayer < 0 ? 0x20 : (1 << topLayer))))
            {
                finalColour = BrightnessIncrease(topColour);
            } else if (blendEff == 3 &&
                (tgt1Mask & (topLayer < 0 ? 0x20 : (1 << topLayer))))
            {
                finalColour = BrightnessDecrease(topColour);
            }

            rowOut[x] = RGB555toRGBA5551(finalColour);
        }
    }
}

/* -----------------------------------------------------------------------
 * ScanlineEffect_ApplyLine — stub called before each scanline
 *
 * The real implementation in src/scanline_effect.c manages per-scanline
 * scroll register arrays (gScanlineEffectRegBuffers).  We call it to
 * update the software IO register file before the compositor reads it.
 *
 * This function is declared here as a weak symbol; scanline_effect.c
 * provides the actual implementation.
 * --------------------------------------------------------------------- */
__attribute__((weak)) void ScanlineEffect_ApplyLine(int line)
{
    (void)line;
}
