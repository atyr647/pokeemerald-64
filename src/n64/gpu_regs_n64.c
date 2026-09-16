/*
 * src/n64/gpu_regs_n64.c
 *
 * N64 port — GPU register manager
 *
 * The original src/gpu_regs.c buffers GBA GPU register writes and flushes
 * them to hardware each VBlank.  On N64 the "hardware" is our software IO
 * register file (gN64IoRegs[]), so there is no separate flush step needed.
 *
 * We provide the same public API (SetGpuReg, GetGpuReg, etc.) so that all
 * game code that calls them compiles and links without modification.
 *
 * SetGpuReg() writes directly to gN64IoRegs[].
 *
 * The full software compositor pipeline is:
 *   1. N64_CompositeFrame()   — tile renderer (tile_renderer.c)
 *   2. N64_CompositeSprites() — sprite renderer (sprite_renderer.c)
 *   3. N64_VISwapBuffers()    — write back the cache, flip framebuffers
 *
 * CopyBufferedValuesToGpuRegs() is called from VBlankIntr(), which runs in
 * interrupt context (see interrupt.c's N64_DispatchIntr).  On real GBA
 * hardware this function is a handful of register writes -- microseconds.
 * Here it is a full software render of a 240x160 frame plus up to 128
 * sprites, measured at up to ~3.7x N64_COUNTS_PER_FRAME (the entire CPU
 * cycle budget of one 60 Hz VBlank period) once real tile/sprite content
 * is loaded.  Running that synchronously inside the interrupt handler is
 * fatal: MI's VI interrupt is level-triggered, so if servicing it takes
 * longer than one VBlank period, the next VBlank is already pending the
 * instant this one returns, and the main thread never regains the CPU --
 * an unbreakable back-to-back interrupt chain indistinguishable from an
 * infinite loop anywhere else in the game (this is exactly how Pokemon
 * Emerald 64's boot sequence stalled forever partway through the
 * copyright screen, once LoadCopyrightGraphics() populated real VRAM
 * content for the compositor to render).
 *
 * So CopyBufferedValuesToGpuRegs() itself just raises a flag (sGN64RenderPending)
 * and returns immediately, keeping the interrupt handler fast regardless of
 * compositor cost.  N64_RunDeferredCompositor(), called once per iteration
 * from WaitForVBlank() in main.c (outside interrupt context), does the
 * actual work.  This decouples "how long rendering takes" from "whether
 * interrupts can be serviced": other interrupts (and the exception
 * mechanism generally) keep working normally while a render is in
 * progress, so the game runs at whatever framerate the compositor can
 * sustain instead of hanging outright once it can't keep up with 60 Hz.
 */

#include "global.h"
#include "gpu_regs.h"
#include "scanline_effect.h"

/* -----------------------------------------------------------------------
 * GPU register buffering state
 *
 * The original gpu_regs.c uses a 2-entry pending queue to handle writes
 * that occur during HBlank DMA.  On N64 we simplify: all writes go
 * directly to the software register file.
 * --------------------------------------------------------------------- */

static u8 sGpuRegBufCount = 0;   /* always 0 on N64 — kept for API compat */

void InitGpuRegManager(void)
{
    sGpuRegBufCount = 0;
}

/* Write a 16-bit value to the GPU register at the given offset */
void SetGpuReg(u8 offset, u16 value)
{
    if (offset < N64_IOREGS_SIZE - 1)
        _REG16(offset) = value;
}

/* Read the current value of a GPU register */
u16 GetGpuReg(u8 offset)
{
    if (offset < N64_IOREGS_SIZE - 1)
        return _REG16(offset);
    return 0;
}

/* Bitwise-OR a value into a GPU register */
void SetGpuRegBits(u8 offset, u16 mask)
{
    if (offset < N64_IOREGS_SIZE - 1)
        _REG16(offset) |= mask;
}

/* Bitwise-AND-NOT a mask from a GPU register (clear bits) */
void ClearGpuRegBits(u8 offset, u16 mask)
{
    if (offset < N64_IOREGS_SIZE - 1)
        _REG16(offset) &= ~mask;
}

/* Set the upper byte of a register (used for DISPSTAT VCount line) */
void SetGpuRegByteUpper(u8 offset, u8 value)
{
    if (offset < N64_IOREGS_SIZE - 1) {
        u16 reg = _REG16(offset);
        _REG16(offset) = (reg & 0x00FF) | ((u16)value << 8);
    }
}

/* Set the lower byte of a register */
void SetGpuRegByteLower(u8 offset, u8 value)
{
    if (offset < N64_IOREGS_SIZE - 1) {
        u16 reg = _REG16(offset);
        _REG16(offset) = (reg & 0xFF00) | value;
    }
}

/* -----------------------------------------------------------------------
 * CopyBufferedValuesToGpuRegs — called each VBlank from main.c's
 * VBlankIntr(), i.e. from interrupt context.
 *
 * On GBA this flushes the pending register write queue to MMIO -- cheap.
 * On N64 it must NOT run the compositor directly (see the file comment
 * above); it just raises sGN64RenderPending for N64_RunDeferredCompositor()
 * to pick up outside interrupt context.
 * --------------------------------------------------------------------- */
static volatile u8 sGN64RenderPending = 0;

void CopyBufferedValuesToGpuRegs(void)
{
    sGN64RenderPending = 1;
}

/* -----------------------------------------------------------------------
 * N64_RunDeferredCompositor — the actual compositor pipeline, run from
 * WaitForVBlank() in main.c, outside interrupt context.  See the file
 * comment above for why this is split out of CopyBufferedValuesToGpuRegs().
 * --------------------------------------------------------------------- */
extern void N64_CompositeFrame(void);      /* tile_renderer.c   */
extern void N64_CompositeSprites(void);    /* sprite_renderer.c */
extern void N64_VISwapBuffers(void);       /* vi.c              */
extern void N64_VIPaintBorders(void);      /* vi.c              */

/* -----------------------------------------------------------------------
 * Compositor profiling overlay
 *
 * There is no console to print to and no profiler to attach, so the timings
 * go out the only channel that always works: the screen. Set this to 1 and
 * each frame draws six 32-bit values as bars of black-and-white cells
 * across the top of the VI framebuffer -- the CP0 cycle counts for the
 * background pass, the sprite pass and the blit, the gap since the previous
 * frame, a frame counter, and BLDCNT/DISPCNT. Screenshot it and run
 * tools/decode_profile.py on the image to read them back.
 *
 * Two screenshots a known number of seconds apart also give the true frame
 * rate from the frame counter, which is worth more than the cycle counts
 * under an emulator that approximates the CPU clock.
 * --------------------------------------------------------------------- */
#define N64_PROFILE_OVERLAY 1
#if N64_PROFILE_OVERLAY
extern u16 *gN64BackBuffer;

static inline u32 C0Count(void)
{
    u32 c;
    asm volatile ("mfc0 %0, $9" : "=r"(c));
    return c;
}

/* The overlay's geometry, shared with tools/decode_profile.py.
 *
 * Everything is inset from the edges of the VI frame: the VI blanks its
 * first eight and last seven output dots, which clipped a full-width marker
 * and threw the decoder's scale off. */
#define OV_X0        16    /* first VI column used                     */
#define OV_CELL       8    /* VI px per cell                           */
#define OV_CELLS     33    /* cell 0 is the ruler, 1-32 are the bits   */
#define OV_MARK_ROW   2    /* VI row the white marker starts  */
#define OV_MARK_ROWS  4    /* how tall the marker is          */
#define OV_BAND_ROW   8    /* VI row the first value band starts */
#define OV_BAND_ROWS  4

/* A white bar of known width and height at a known row. It is how the
 * decoder finds the VI frame inside a screenshot without knowing anything
 * about the emulator's window: the bar's ends are VI x=OV_X0 and
 * x=OV_X0+32*OV_CELL-1, and its height is OV_MARK_ROWS rows. */
static void ProfileMarker(void)
{
    for (int r = 0; r < OV_BAND_ROW; r++) {
        u16 *p = gN64BackBuffer + r * N64_VI_WIDTH;
        int white = (r >= OV_MARK_ROW && r < OV_MARK_ROW + OV_MARK_ROWS);
        for (int i = 0; i < N64_VI_WIDTH; i++)
            p[i] = 0x0001;
        if (white) {
            for (int i = 0; i < OV_CELLS * OV_CELL; i++)
                p[OV_X0 + i] = 0xFFFF;
        }
    }
}

/* Cell 0 of every band is a white stripe, so the decoder finds each band's
 * rows by looking for stripes rather than working them out from a scale --
 * ares corrects the N64's pixel aspect and mupen64plus does not, so a
 * vertical scale inferred from the horizontal one drifts a whole band by the
 * bottom of the overlay. It sits inside the marker's span so that whatever
 * an emulator crops or scales, finding the marker is enough to find it. */
static void ProfileRuler(int band)
{
    u16 *fb = gN64BackBuffer + (OV_BAND_ROW + band * OV_BAND_ROWS) * N64_VI_WIDTH;
    for (int r = 0; r < OV_BAND_ROWS; r++) {
        u16 *p = fb + r * N64_VI_WIDTH;
        u16 c = (r < OV_BAND_ROWS - 1) ? 0xFFFF : 0x0001;
        for (int i = 0; i < OV_CELL; i++)
            p[OV_X0 + i] = c;
    }
}

/* Paint a 32-bit value as 32 cells, one band of the VI framebuffer.
 * White = 1, dark grey = 0, so a screenshot decodes back to the number. */
static void ProfileBits(int band, u32 value)
{
    u16 *fb = gN64BackBuffer + (OV_BAND_ROW + band * OV_BAND_ROWS) * N64_VI_WIDTH;

    ProfileRuler(band);

    for (int r = 0; r < OV_BAND_ROWS; r++) {
        u16 *p = fb + r * N64_VI_WIDTH;
        for (int bit = 0; bit < 32; bit++) {
            u16 c = (value & (1u << (31 - bit))) ? 0xFFFF : 0x2109;
            for (int i = 0; i < OV_CELL; i++)
                p[OV_X0 + (1 + bit) * OV_CELL + i] = c;
        }
    }
}
#endif
/* -------------------------------------------------------------------- */

/* TEMP: RDP viability spike.
 *
 * Counts how many composited frames use a feature a display-list renderer
 * could not draw straight through: a per-scanline register effect, an
 * affine background, a window, or an 8bpp layer. Whatever fraction that
 * comes to is the fraction of frames that would still need the software
 * compositor, and so the ceiling on what moving to the RDP is worth. */
u32 gSpikeFrames, gSpikeScanline, gSpikeAffine, gSpikeWindow, gSpikeBpp8;

static void SpikeSample(void)
{
    extern struct ScanlineEffect gScanlineEffect;
    u16 dispcnt = _REG16(REG_OFFSET_DISPCNT);
    int mode = dispcnt & 7;

    gSpikeFrames++;

    if (gScanlineEffect.state != 0)
        gSpikeScanline++;
    if (mode != 0)
        gSpikeAffine++;
    if ((dispcnt >> 13) & 3)
        gSpikeWindow++;

    static const int bgCnt[4] = { REG_OFFSET_BG0CNT, REG_OFFSET_BG1CNT,
                                  REG_OFFSET_BG2CNT, REG_OFFSET_BG3CNT };
    for (int i = 0; i < 4; i++) {
        if (((dispcnt >> (8 + i)) & 1) && ((_REG16(bgCnt[i]) >> 7) & 1)) {
            gSpikeBpp8++;
            break;
        }
    }
}

void N64_RunDeferredCompositor(void)
{
    if (!sGN64RenderPending)
        return;
    sGN64RenderPending = 0;

    N64_VIPaintBorders();
    SpikeSample();

#if N64_PROFILE_OVERLAY
    /* A fixed window of game time, accumulated and then frozen.
     *
     * Reading one frame's cost compares nothing: the emulators run at
     * different speeds, so a screenshot taken after the same wall-clock time
     * catches them at different points in the intro.
     *
     * The window is counted in VBlanks rather than composited frames. Game
     * logic advances once per VBlank whatever the compositor is doing, so a
     * span of VBlanks is the same span of the same scene on both emulators;
     * a span of composited frames is not, because the slower emulator has
     * let more game time pass by the time it has drawn as many. Frames
     * composited inside the window are counted separately -- that is the
     * frame rate over a known stretch of the game. */
    #define BENCH_FIRST  600    /* 10 s of game time, past the boot sequence */
    #define BENCH_LAST  3600    /* through to 60 s                           */

    extern volatile u32 gN64VBlankCount;

    static u32 sPrevEnd = 0;
    static u32 sFrames = 0;
    static u32 sBgTotal = 0, sSprTotal = 0, sIdleTotal = 0;

    ProfileMarker();

    u32 t0 = C0Count();
    N64_CompositeFrame();
    u32 t1 = C0Count();
    N64_CompositeSprites();
    u32 t2 = C0Count();
    u32 t3 = t2;

    u32 vblank = gN64VBlankCount;
    if (vblank >= BENCH_FIRST && vblank < BENCH_LAST) {
        sBgTotal   += t1 - t0;
        sSprTotal  += t2 - t1;
        sIdleTotal += t0 - sPrevEnd;
        sFrames++;
    }

    ProfileBits(0, sBgTotal);           /* backgrounds, summed over the window */
    ProfileBits(1, sSprTotal);          /* sprites                             */
    ProfileBits(2, t3 - t2);            /* unused                              */
    ProfileBits(3, sIdleTotal);         /* game logic between frames           */
    ProfileBits(4, sFrames);            /* frames composited inside the window */
    ProfileBits(5, ((u32)_REG16(REG_OFFSET_BLDCNT) << 16)
                 | (u32)_REG16(REG_OFFSET_DISPCNT));
    ProfileBits(6, gSpikeFrames);
    ProfileBits(7, gSpikeScanline);
    ProfileBits(8, gSpikeAffine);
    ProfileBits(9, gSpikeWindow);
    ProfileBits(10, gSpikeBpp8);
    sPrevEnd = t3;

    N64_VISwapBuffers();
#else
    /* Run the full software compositor */
    N64_CompositeFrame();
    N64_CompositeSprites();
    N64_VISwapBuffers();
#endif

    /* Update VCOUNT to match current VI line */
    extern volatile u16 gN64CurrentLine;
    _REG16(REG_OFFSET_VCOUNT) = gN64CurrentLine;
}

/* -----------------------------------------------------------------------
 * EnableInterrupts / DisableInterrupts
 * Declared in gpu_regs.h; implemented here for N64.
 * --------------------------------------------------------------------- */
void EnableInterrupts(u16 mask)
{
    N64_IntrEnable(mask);
}

void DisableInterrupts(u16 mask)
{
    /* On N64 we don't currently disable individual interrupt sources;
     * the game uses this for temporary critical sections which are
     * short enough that we can ignore. */
    (void)mask;
}

/* -----------------------------------------------------------------------
 * SetGpuReg_ForcedBlank — sets a register while also enabling forced blank.
 * On N64, forced blank just means we clear screen (the compositor handles it).
 * --------------------------------------------------------------------- */
void SetGpuReg_ForcedBlank(u8 regOffset, u16 value)
{
    /* Enable forced blank in DISPCNT */
    _REG16(REG_OFFSET_DISPCNT) |= DISPCNT_FORCED_BLANK;
    /* Write the target register */
    SetGpuReg(regOffset, value);
}
