/*
 * src/n64/vi.c
 *
 * N64 port — Video Interface (VI) initialisation and framebuffer management
 *
 * The N64 VI drives the display from a framebuffer in RDRAM.  We configure
 * it for 320×240 @ 16-bit colour (RGBA5551 — N64 native 16-bit format).
 *
 * The GBA renders at 240×160.  We allocate a 240×160 internal render buffer
 * that the tile/sprite compositor writes to, then blit it into the centre
 * of the 320×240 VI framebuffer (40-pixel black borders top/bottom).
 *
 * Double-buffering: two 320×240 framebuffers live at the top of RDRAM
 * (__fb0_start, __fb1_start).  The compositor writes to the back buffer;
 * after each VBlank the buffers are flipped by updating VI_ORIGIN.
 *
 * Endianness note:
 *   The port is built big-endian (-EB), same as the VI's registers, so
 *   N64_HW_WR() is a plain volatile 32-bit store with no swapping.  The
 *   framebuffer is 16-bit RGBA5551 and is likewise written natively.
 */

#include <string.h>
#include "global.h"
#include "n64/asm_defs.h"
#include "n64/defines.h"

/* -----------------------------------------------------------------------
 * VI register access wrappers (LE CPU → BE hardware)
 * --------------------------------------------------------------------- */
#define VI_WR(off, val)  N64_HW_WR(N64_VI_BASE_REG, (off), (uint32_t)(val))
#define VI_RD(off)       N64_HW_RD(N64_VI_BASE_REG, (off))

/* -----------------------------------------------------------------------
 * Framebuffer pointers (exported so the compositor can use them)
 * --------------------------------------------------------------------- */
extern u8 __fb0_start[];
extern u8 __fb1_start[];

/* Currently displayed framebuffer (0 or 1) */
static int sDisplayFB  = 0;

/* Back buffer (compositor writes here) */
u16 *gN64BackBuffer  = NULL;
u16 *gN64FrontBuffer = NULL;

/* 240×160 GBA render target — compositor fills this, then we blit to VI FB */
u16 gN64GBAFramebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

static void paint_borders(u16 *fb);

/* -----------------------------------------------------------------------
 * N64_InitVI — configure the Video Interface for 320×240 16-bit NTSC
 * --------------------------------------------------------------------- */
void N64_InitVI(void)
{
    /* Use KSEG1 (uncached) pointers so writes bypass D-cache and reach RDRAM
     * immediately — VI reads RDRAM directly, not the D-cache.            */
    gN64FrontBuffer = (u16 *)((uintptr_t)__fb0_start | 0x20000000u);
    gN64BackBuffer  = (u16 *)((uintptr_t)__fb1_start | 0x20000000u);
    sDisplayFB      = 0;

    /* Clear both framebuffers to black (via KSEG1 — must reach RDRAM) */
    memset((void *)((uintptr_t)__fb0_start | 0x20000000u), 0, N64_VI_WIDTH * N64_VI_HEIGHT * sizeof(u16));
    memset((void *)((uintptr_t)__fb1_start | 0x20000000u), 0, N64_VI_WIDTH * N64_VI_HEIGHT * sizeof(u16));

    /* VI_STATUS: 16-bit colour, no gamma, no divot, progressive.
     *
     * Bits 9:8 are the AA/resample mode:
     *
     *   0  resample + anti-alias, always fetch extra lines
     *   1  resample + anti-alias, fetch extra lines when needed
     *   2  resample only -- every pixel treated as fully covered
     *   3  replicate -- point sample, no interpolation at all
     *
     * Mode 3 is the one you want for a 2D image: it reproduces exactly
     * what the compositor wrote. It is also unusable here, because the VI
     * only buffers 64 pixels per line in that mode, and with a 320-pixel
     * framebuffer scaled 2x (X_SCALE 0x200) into the standard 640-dot
     * active area starting at dot 108 it wraps: the hardware shows source
     * pixels 0-63, then 64 dot-pairs of black, then pixels 0-63 again, and
     * so on across the line. The picture comes out as three narrow copies
     * of its own left edge. angrylion reproduces this faithfully (see the
     * `vinnglitch` path in its vi.c, armed when aa_mode is 3, the type has
     * bit 1 set, H_START < 0x80 and X_SCALE <= 0x200), so it is hardware
     * behaviour and not an emulator quirk -- a console does the same.
     *
     * Mode 2 is the fix. It reads the framebuffer raw, exactly like mode 3
     * -- no coverage bits, no AA filter -- and only adds the horizontal
     * resample. Since X_SCALE is exactly 0x200, that resample is a plain
     * 2x linear upscale: even output dots are a source pixel untouched,
     * odd ones are the average of that pixel and the next. Nothing is lost
     * and no filter runs over the image.
     *
     * Modes 0 and 1 are the ones to stay away from. They take each pixel's
     * coverage from its alpha bit plus the hidden RDRAM bits, and the
     * compositor has no meaningful coverage to give them, so they run the
     * AA filter over a fully-opaque image and smear one-pixel features
     * into their neighbours. */
    VI_WR(VI_STATUS_REG,  0x00003202);

    /* VI_ORIGIN: physical RDRAM address of front framebuffer */
    VI_WR(VI_ORIGIN_REG,  (u32)((uintptr_t)__fb0_start & 0x00FFFFFF));

    /* VI_WIDTH: line width in pixels */
    VI_WR(VI_WIDTH_REG,   N64_VI_WIDTH);

    /* VI_INTR: interrupt at half-line 2 — fires once per frame at the very
     * start of the next field, used as our VBlank event.               */
    VI_WR(VI_INTR_REG,    0x00000002);

    /* VI_CURRENT: clear */
    VI_WR(0x10, 0);

    /* VI_BURST: colour burst — standard NTSC */
    VI_WR(VI_BURST_REG,   0x03E52239);

    /* VI_V_SYNC: NTSC — 525 half-lines */
    VI_WR(VI_V_SYNC_REG,  0x0000020D);

    /* VI_H_SYNC: NTSC — 3093 pixels per line */
    VI_WR(VI_H_SYNC_REG,  0x00000C15);

    /* VI_LEAP */
    VI_WR(VI_LEAP_REG,    0x0C150C15);

    /* VI_H_START: [109, 749] for 320-pixel output */
    VI_WR(VI_H_START_REG, 0x006C02EC);

    /* VI_V_START: [37, 511] half-lines */
    VI_WR(VI_V_START_REG, 0x002501FF);

    /* VI_V_BURST */
    VI_WR(VI_V_BURST_REG, 0x000E0204);

    /* VI_X_SCALE: 1:1 horizontal (no scale) */
    VI_WR(VI_X_SCALE_REG, 0x00000200);

    /* VI_Y_SCALE: 1:1 vertical */
    VI_WR(VI_Y_SCALE_REG, 0x00000400);
}

/* -----------------------------------------------------------------------
 * N64_VISwapBuffers — flip front/back buffers at VBlank
 * --------------------------------------------------------------------- */
void N64_VISwapBuffers(void)
{
    u8 *newFront = (sDisplayFB == 0) ? __fb1_start : __fb0_start;
    u32 physAddr = (u32)((uintptr_t)newFront & 0x00FFFFFF);

    VI_WR(VI_ORIGIN_REG, physAddr);

    if (sDisplayFB == 0) {
        sDisplayFB      = 1;
        gN64FrontBuffer = (u16 *)((uintptr_t)__fb1_start | 0x20000000u);
        gN64BackBuffer  = (u16 *)((uintptr_t)__fb0_start | 0x20000000u);
    } else {
        sDisplayFB      = 0;
        gN64FrontBuffer = (u16 *)((uintptr_t)__fb0_start | 0x20000000u);
        gN64BackBuffer  = (u16 *)((uintptr_t)__fb1_start | 0x20000000u);
    }
}

/* -----------------------------------------------------------------------
 * N64_VISetVCountLine — configure VI interrupt scanline
 * --------------------------------------------------------------------- */
void N64_VISetVCountLine(u16 line)
{
    VI_WR(VI_INTR_REG, (u32)line);
}

/* -----------------------------------------------------------------------
 * N64_BlitGBAFrame — copy 240×160 render target into 320×240 back buffer
 *
 * Centres the GBA picture with black letterbox/pillarbox borders.
 * --------------------------------------------------------------------- */
/* Fill helper: write opaque black (RGBA5551 = 0x0001) to n pixels.
 * memset fills bytes; 0x0001 is not a repeated byte pattern, so this writes
 * whole words where it can -- the borders are pixel-pair aligned. */
static void fill_opaque_black(u16 *dst, int n)
{
    while (n > 0 && ((uintptr_t)dst & 3)) { *dst++ = 0x0001u; n--; }
    u32 *w = (u32 *)dst;
    while (n >= 2) { *w++ = 0x00010001u; n -= 2; }
    dst = (u16 *)w;
    while (n-- > 0) *dst++ = 0x0001u;
}

/* The letterbox is the same every frame, so it is painted once per buffer
 * rather than 76,800 pixels of it being rewritten 60 times a second. */
static void paint_borders(u16 *fb)
{
    fill_opaque_black(fb, N64_FB_Y_OFFSET * N64_VI_WIDTH);
    u16 *row = fb + N64_FB_Y_OFFSET * N64_VI_WIDTH;
    for (int y = 0; y < DISPLAY_HEIGHT; y++, row += N64_VI_WIDTH) {
        fill_opaque_black(row, N64_FB_X_OFFSET);
        fill_opaque_black(row + N64_FB_X_OFFSET + DISPLAY_WIDTH, N64_FB_X_OFFSET);
    }
    fill_opaque_black(row, N64_FB_Y_OFFSET * N64_VI_WIDTH);
}

void N64_BlitGBAFrame(void)
{
    /* Paint the letterbox on the first frame rather than in N64_InitVI():
     * N64Main()'s boot diagnostics fill the whole framebuffer several more
     * times after VI init, so anything laid down earlier is painted over
     * and the borders would come up whatever colour the last DIAG left. */
    static int bordersDone = 0;
    if (!bordersDone) {
        paint_borders(gN64FrontBuffer);
        paint_borders(gN64BackBuffer);
        bordersDone = 1;
    }

    const u16 *src = gN64GBAFramebuffer;
    u16       *dst = gN64BackBuffer
                   + N64_FB_Y_OFFSET * N64_VI_WIDTH + N64_FB_X_OFFSET;

    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        memcpy(dst, src, DISPLAY_WIDTH * sizeof(u16));
        dst += N64_VI_WIDTH;
        src += DISPLAY_WIDTH;
    }
}
