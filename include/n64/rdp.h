#ifndef GUARD_N64_RDP_H
#define GUARD_N64_RDP_H

/*
 * include/n64/rdp.h
 *
 * N64 port — Reality Display Processor command-list builder
 *
 * The RDP is a second processor with its own rasterizer and no CPU-visible
 * pixels: fills, texture blits and triangles are commands, 64 bits apiece,
 * that the CPU writes into a buffer in RDRAM and hands to the DP by pointing
 * DPC_START/DPC_END at the span. This is the whole interface -- no RSP
 * microcode is involved, and none is needed for 2D work. libultra and
 * libdragon route even 2D through the RSP because their display lists are
 * built once and replayed every frame from a task queue; this port rebuilds
 * its display list from scratch each frame anyway (the scene changes every
 * frame), so the RSP hop buys nothing here and the CPU talks to the DP
 * directly.
 *
 * The opcode values and field layouts below are cross-checked against a
 * second, independent CPU-built-display-list implementation (github.com/
 * atyr647/pak, runtime/standalone/runtime.pk64) that exercises the same
 * registers on real hardware and on ares, including several gotchas that
 * cost that project real debugging time -- noted at their use site here so
 * they are not rediscovered:
 *   - TEXTURE_RECTANGLE's bottom-right corner is inclusive in COPY/FILL
 *     cycle but exclusive in 1-/2-cycle mode.
 *   - COPY cycle retires four pixels per cycle, so its S step is written
 *     four times the 1-cycle value.
 *   - The texture filter's bilerp bits must be set even when not
 *     filtering, or the RDP treats the texture as YUV and returns black.
 *   - A triangle's Y coordinates are a signed 11.2 field, not the unsigned
 *     10.2 field SET_SCISSOR and the rectangle commands use.
 */

#include "global.h"

/* -----------------------------------------------------------------------
 * Display-list submission
 * --------------------------------------------------------------------- */
void RDP_Init(void);

/* Hands whatever is buffered to the DP and blocks until it drains. Called
 * automatically when a command would overflow the buffer, and must be
 * called at the end of a frame's drawing before the CPU or VI touches
 * anything the RDP was told to write. */
void RDP_Submit(void);

/* Forces a submit first if the current half-buffer has less than `bytes`
 * free. Call this before a group of commands that must land in the same
 * batch -- a texture load followed by the draw that reads it, say -- so
 * the auto-submit inside each individual command can never fall in the
 * middle of the group. `bytes` need not be exact; padding it costs
 * nothing but reserving too little defeats the point. */
void RDP_Reserve(int bytes);

/* -----------------------------------------------------------------------
 * Render targets and clipping
 * --------------------------------------------------------------------- */
/* fmt: 0=RGBA, 2=CI. size: 0=4bpp, 1=8bpp, 2=16bpp, 3=32bpp. */
void RDP_SetColorImage(u32 addr, u32 fmt, u32 size, int width);
void RDP_SetScissor(int x0, int y0, int x1, int y1);

/* -----------------------------------------------------------------------
 * Render modes
 * --------------------------------------------------------------------- */
void RDP_SetModeFill(u16 rgba5551);
void RDP_SetModeCopy(void);
/* 1-cycle, texture passed through unmodified, alpha-blended over what is
 * already in the framebuffer. */
void RDP_SetModeStandard(void);
/* Same, but samples through the TLUT (en_tlut, tlut_type=RGBA16) instead
 * of taking the texel as a direct colour -- what every CI4/CI8 draw needs,
 * on top of RDP_SetModeStandard's blend and filter setup. */
void RDP_SetModeStandardTlut(void);
void RDP_SyncPipe(void);
void RDP_SyncTile(void);
void RDP_SyncLoad(void);
void RDP_SyncFull(void);

void RDP_SetFillColor16(u16 rgba5551);
void RDP_FillRectangle(int x0, int y0, int x1, int y1);

/* -----------------------------------------------------------------------
 * Texturing
 *
 * One TMEM (4KB) holds either 2048 CI4 texels (a TLUT halves that, to 2KB /
 * 1024 texels, since the palette shares the same memory) or 2048 RGBA16
 * texels. A GBA 4bpp charblock tile is 32 bytes -- 64 texels -- so a TLUT
 * page holds 16 of them.
 * --------------------------------------------------------------------- */
/* fmt: 0=RGBA, 2=CI. size: 0=4bpp, 1=8bpp, 2=16bpp, 3=32bpp. addr must be a
 * physical RDRAM address the DP can read -- the caller is responsible for
 * writing back any CPU cache line covering it first (see rdp.c). */
void RDP_SetTextureImage(u32 addr, u32 fmt, u32 size, int width);

/* line: TMEM bytes per texel row. tmemAddr: TMEM byte offset, /8. palette:
 * 4-bit bank, meaningful only for CI4 (matches SET_TILE's own field, which
 * is where a GBA 4bpp tile's palette bank goes). */
void RDP_SetTile(int tile, u32 fmt, u32 size, int line, int tmemAddr, int palette);
void RDP_SetTileSize(int tile, int s0, int t0, int s1, int t1);
void RDP_LoadTile(int tile, int s0, int t0, int s1, int t1);

/* Loads `count` 16-bit TLUT entries starting at TMEM palette slot `first`
 * (0-255) from the last SET_TEXTURE_IMAGE. Each entry must be RGBA5551,
 * already converted -- LOAD_TLUT copies bytes, it does not convert. */
void RDP_LoadTlut(int tile, int first, int count);

/* A 1:1 blit, tile's currently-loaded texels to (x0,y0)-(x1,y1). Call
 * RDP_SetModeCopy() first for an opaque blit or RDP_SetModeStandard() to
 * alpha-blend. */
void RDP_TextureRectangle(int tile, int x0, int y0, int x1, int y1, int s, int t);

/* The general form: an explicit starting position, in HALF-texel units (a
 * whole texel is 2 -- the extra precision is for callers that need to
 * offset a sample by half a texel; see RDP_TextureRectangle), and a
 * per-pixel step in each axis (s5.10 fixed point -- 1024 is one texel), so
 * a negative step reads a tile backwards for a horizontal or vertical
 * flip. RDP_TextureRectangle is this with the unflipped 1:1 case's
 * position and steps filled in. Only valid in 1-/2-cycle mode: COPY's
 * four-texels-per-cycle stepping does not have a sensible reverse
 * direction. */
void RDP_TextureRectangleXF(int tile, int x0, int y0, int x1, int y1,
                             int sHalf, int tHalf, int dsdx, int dtdy);

/* Writes back the CPU data cache over [addr, addr+len) unless addr is
 * already an uncached (KSEG1) alias. Every texture source the RDP reads --
 * GBA VRAM, the TLUT built from GBA palette RAM -- is CPU-written and
 * cached, so this has to run before the matching LOAD_TILE/LOAD_TLUT or the
 * DP samples whatever RDRAM had before the CPU's last write. */
void RDP_WritebackSource(const void *addr, int len);

/* Discards the CPU data cache's copy of [addr, addr+len) without writing it
 * back. Call this once RDP_Submit() has drained a draw the RDP made into a
 * framebuffer region the CPU's cache holds a (now stale) copy of: vi.c's
 * once-a-frame whole-cache writeback is unconditional, and without this it
 * flushes that stale copy over the RDP's fresh RDRAM data on the next
 * buffer swap. */
void RDP_InvalidateDest(const void *addr, int len);

#endif /* GUARD_N64_RDP_H */
