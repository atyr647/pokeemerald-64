/*
 * src/n64/platform.c
 *
 * N64 port — platform initialisation and main entry point
 *
 * N64Main() is called from crt0.s after BSS is cleared.
 * It initialises all N64 hardware subsystems, then calls AgbMain()
 * which is the original Pokémon Emerald entry point in src/main.c.
 * AgbMain() never returns.
 */

#include <string.h>
#include "global.h"
#include "n64/asm_defs.h"
#include "n64/defines.h"
#include "main.h"
#include "malloc.h"

/* -----------------------------------------------------------------------
 * IS-Viewer64 debug output (works on emulators with ISV support)
 * Physical 0x13FF0014 = write-length trigger
 * Physical 0x13FF0020 = string buffer
 * CPU and PI bus are both big-endian (-EB); write directly, no bswap.
 * --------------------------------------------------------------------- */
void N64_DebugPrint(const char *str)
{
    volatile u32 *isv_len = (volatile u32 *)0xB3FF0014;
    volatile u8  *isv_buf = (volatile u8  *)0xB3FF0020;
    u32 len = 0;
    while (str[len]) { isv_buf[len] = (u8)str[len]; len++; }
    isv_buf[len++] = '\n';
    *isv_len = len;
}

/* -----------------------------------------------------------------------
 * Linker-exported symbols (from n64.ld)
 * --------------------------------------------------------------------- */
extern u8  __sw_palette_start[];
extern u8  __sw_vram_start[];
extern u8  __sw_oam_start[];
extern u8  __sw_ioregs_start[];
extern u8  __heap_start[];
extern u8  __stack_bottom[];
extern u8  __fb0_start[];
extern u8  __fb1_start[];
extern u8  __bss_start[];
extern u8  __bss_end[];

/* -----------------------------------------------------------------------
 * Software hardware buffer pointers (referenced by defines.h macros)
 * --------------------------------------------------------------------- */
void *__n64_pltt_buf = NULL;
void *__n64_vram_buf = NULL;
void *__n64_oam_buf  = NULL;

/* Software I/O register file */
u8 gN64IoRegs[N64_IOREGS_SIZE];

/* GBA defines.h global variable stubs */
struct SoundInfo *__n64_sound_info_ptr = NULL;
volatile u16      __n64_intr_check     = 0;
void             *__n64_intr_vector    = NULL;

/* -----------------------------------------------------------------------
 * N64 hardware register helpers — byte-swap wrappers for big-endian MMIO
 * --------------------------------------------------------------------- */
/* N64_HW_RD / N64_HW_WR are defined in n64/asm_defs.h */

/* N64_InitMI removed — MI_MODE write (0x0500) stalls the SC64 PI bus.
 * MI_INTR_MASK is written inline in N64Main() after the YELLOW diagnostic.
 * SC64 IPL3 already configured MI_MODE correctly; we must not touch it.

 * N64_InitRI removed — do NOT reinitialize the RDRAM Interface after IPL3.
 * The HLE IPL3 (SC64 firmware / emulator) sets RI registers correctly.
 * Writing RI_CONFIG/RI_REFRESH with wrong values instantly corrupts all
 * RDRAM access, crashing the CPU before a single frame is rendered. */

/* -----------------------------------------------------------------------
 * N64_InitSP — Signal Processor
 * Halt the RSP; we do not use it for custom microcode in this port.
 * --------------------------------------------------------------------- */
static void N64_InitSP(void)
{
    /* Set SP_STATUS: halt RSP, clear broke, clear interrupt */
    N64_HW_WR(N64_SP_BASE_REG, 0x10, 0x0E);   /* SET_HALT(b1)|CLR_BROKE(b2)|CLR_INTR(b3) */
    /* Wait for RSP to halt with timeout — avoids infinite spin if RSP is stuck */
    for (int i = 0; i < 2000000; i++) {
        if (N64_HW_RD(N64_SP_BASE_REG, 0x10) & 1)
            break;
    }
}

/* -----------------------------------------------------------------------
 * N64_InitPI — Peripheral Interface (cartridge bus)
 * Standard bus timing values for commercial N64 carts.
 * --------------------------------------------------------------------- */
static void N64_InitPI(void)
{
    N64_HW_WR(N64_PI_BASE_REG, PI_STATUS_REG,       3);     /* clear DMA busy/error */
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_LAT_REG, 0x40);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_PWD_REG, 0x12);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_PGS_REG, 0x07);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_RLS_REG, 0x03);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_LAT_REG, 0x05);  /* FlashRAM domain */
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_PWD_REG, 0x0C);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_PGS_REG, 0x02);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_RLS_REG, 0x02);
}

/* -----------------------------------------------------------------------
 * N64_EnableCPUInterrupts — turn on MIPS interrupt handling
 * --------------------------------------------------------------------- */
static void N64_EnableCPUInterrupts(void)
{
    /* CP0 Status: clear BEV so interrupts go to our handlers at 0x80000180
     * (not PIF ROM at 0xBFC00380), then set IE and IM2.
     * BEV = bit 22 = 0x00400000.  crt0.s was supposed to clear it but had
     * a wrong mask (~0x00010001 instead of ~0x00400007). */
    u32 sr;
    asm volatile (
        "mfc0  %0, $12\n\t"
        "ori   %0, %0, 0x0401\n\t"     /* IE=1, IM2=1 */
        "and   %0, %0, ~0x00400006\n\t" /* clear BEV (bit22), EXL (bit1), ERL (bit2) */
        "mtc0  %0, $12\n\t"
        : "=r"(sr)
    );
}

/* -----------------------------------------------------------------------
 * Forward declarations for subsystems
 * --------------------------------------------------------------------- */
extern void N64_InitVI(void);   /* vi.c      */
extern void N64_InitAI(void);   /* audio.c   */
extern void N64_InitInput(void);/* input.c   */
extern void N64_InitFlashRAM(void); /* flashram.c */
extern void RDP_Init(void);     /* rdp.c     */

/* -----------------------------------------------------------------------
 * N64Main — platform entry point (called from crt0.s)
 * --------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * N64_PifTerminateBoot — tell the PIF that the boot process has finished.
 *
 * The PIF halts the CPU roughly 5 seconds after reset unless bit 3 of
 * PIF-RAM byte 0x3F is set.  Nintendo's IPL3 does not send this — it is left
 * to the game (libultra's boot code does it) — and libdragon only sends it
 * from its full ELF loader (boot/loader.c stage3), NOT from the flat-binary
 * compat loader we boot with (boot/loader_compat.c stage3).  So it is on us.
 *
 * Symptom when missing: the console runs normally for ~5 seconds and then
 * freezes on whatever was last drawn, with no other diagnostic.  ares reports
 * it as "[PIF::main] boot timeout: CPU has not sent the boot termination
 * command within 5 seconds. Halting the CPU".
 *
 * Must therefore be the first thing we do, before the (slow) screen fills.
 * --------------------------------------------------------------------- */
#define SI_STATUS_DMA_BUSY 0x0001u
#define SI_STATUS_IO_BUSY  0x0002u

static void N64_PifTerminateBoot(void)
{
    /* Wait for the SI to go idle before touching PIF-RAM. */
    while (N64_HW_RD(N64_SI_BASE_REG, SI_STATUS_REG)
           & (SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY))
        ;

    /* PIF-RAM is 64 bytes at 0xBFC007C0; byte 0x3F is the command byte, so
     * the containing word is at 0xBFC007FC.  Bit 3 = "boot terminated". */
    /* crt0.s already sent this before the long ROM-to-RDRAM copies, since
     * the PIF's five-second deadline expires partway through them. Repeating
     * it here is harmless and keeps the sequence obvious. */
    *(volatile u32 *)0xBFC007FCu = 0x08u;
}

/* Minimal VI init + screen fill used for staged boot diagnostics */
static void DiagFillScreen(u16 colour)
{
    /* Use __fb0_start (linker-allocated at 0x807B0000).
     * Convert KSEG0 → KSEG1 (uncached) so VI writes reach RDRAM immediately.
     * Physical address = virtual & 0x1FFFFFFF */
    u16 *fb = (u16*)((uintptr_t)__fb0_start | 0x20000000u);
    u32 physFB = (u32)((uintptr_t)__fb0_start & 0x1FFFFFFFu);
    N64_HW_WR(N64_VI_BASE_REG, VI_STATUS_REG,  0x00003202);
    N64_HW_WR(N64_VI_BASE_REG, VI_ORIGIN_REG,  physFB);
    N64_HW_WR(N64_VI_BASE_REG, VI_WIDTH_REG,   320);
    N64_HW_WR(N64_VI_BASE_REG, VI_INTR_REG,    0x00000002);
    N64_HW_WR(N64_VI_BASE_REG, VI_BURST_REG,   0x03E52239);
    N64_HW_WR(N64_VI_BASE_REG, VI_V_SYNC_REG,  0x0000020D);
    N64_HW_WR(N64_VI_BASE_REG, VI_H_SYNC_REG,  0x00000C15);
    N64_HW_WR(N64_VI_BASE_REG, VI_LEAP_REG,    0x0C150C15);
    N64_HW_WR(N64_VI_BASE_REG, VI_H_START_REG, 0x006C02EC);
    N64_HW_WR(N64_VI_BASE_REG, VI_V_START_REG, 0x002501FF);
    N64_HW_WR(N64_VI_BASE_REG, VI_V_BURST_REG, 0x000E0204);
    N64_HW_WR(N64_VI_BASE_REG, VI_X_SCALE_REG, 0x00000200);
    N64_HW_WR(N64_VI_BASE_REG, VI_Y_SCALE_REG, 0x00000400);
    for (int i = 0; i < 320 * 240; i++) fb[i] = colour;
}

void N64Main(void)
{
    /* DIAG: fill fb0 via KSEG1 (uncached) so VI sees it immediately.
     * volatile u16* prevents GCC -O2 from eliminating consecutive fills
     * as dead stores. Each fill has a distinct hardware-register write
     * between it and the next, so they can never be merged.
     *
     * Boot colour legend (last colour on screen = last step completed):
     *   BLUE    0x003F  N64Main() reached (set by DiagFillScreen)
     *   YELLOW  0xFFC1  memsets done
     *   CYAN    0x07FF  MI_MODE write done
     *   RED     0xF801  MI_INTR_MASK write done
     *   ORANGE  0xFBC1  PI_STATUS (CLR_INTR) write done
     *   PURPLE  0x783F  DOM1 timing writes done
     *   MAGENTA 0xF83F  DOM2 timing writes done
     *   WHITE   0xFFFF  InitSP done
     *   GREEN   0x07C1  InitVI done
     *   BLUE    0x003F  InitAI done
     *   CYAN    0x07FF  InitInput done
     *   TEAL    0x07E1  about to enable interrupts (IE still 0)
     *   YELLOW  0xFFC1  interrupts enabled + first handler returned
     *   (game starts after YELLOW)
     */
#define DIAG(c) do { \
    volatile u16 *__fb = (volatile u16*)((uintptr_t)__fb0_start | 0x20000000u); \
    for (int __i = 0; __i < 320*240; __i++) __fb[__i] = (c); \
} while(0)

    /* Stop the PIF from halting us ~5 s from now.  Must come before the
     * screen fills below, which are slow (76800 uncached writes each). */
    N64_PifTerminateBoot();

    /* BLUE = N64Main reached */
    DiagFillScreen(0x003F);

    __n64_pltt_buf = __sw_palette_start;
    __n64_vram_buf = __sw_vram_start;
    __n64_oam_buf  = __sw_oam_start;

    memset(__n64_pltt_buf, 0, 0x400);
    memset(__n64_vram_buf, 0, 0x18000);
    memset(__n64_oam_buf,  0, 0x400);
    memset(gN64IoRegs,     0, sizeof(gN64IoRegs));
    *(u16 *)(gN64IoRegs + 0x130) = 0x03FF;

    /* YELLOW = memsets done */
    DIAG(0xFFC1);

    /* MI init — do NOT write MI_MODE (0x0500 sets EBUS_TEST_MODE which stalls
     * the SC64 bus).  SC64 IPL3 already configured MI correctly; we only need
     * to unmask the interrupts we care about. */
    /* CYAN = about to write MI_INTR_MASK */
    DIAG(0x07FF);

    N64_HW_WR(N64_MI_BASE_REG, MI_INTR_MASK_REG,
          (1 << 3)   /* SET_SI */
        | (1 << 5)   /* SET_AI */
        | (1 << 7)   /* SET_VI */
        | (1 << 9)); /* SET_PI */
    /* RED = MI_INTR_MASK write done */
    DIAG(0xF801);

    /* PI init inline — CLR_INTR only (bit 1); do NOT write RESET_CONTROLLER
     * (bit 0) as that can stall the SC64 PI bus indefinitely. */
    N64_HW_WR(N64_PI_BASE_REG, PI_STATUS_REG, 2);
    /* ORANGE = PI_STATUS done */
    DIAG(0xFBC1);

    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_LAT_REG, 0x40);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_PWD_REG, 0x12);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_PGS_REG, 0x07);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM1_RLS_REG, 0x03);
    /* PURPLE = DOM1 timing done */
    DIAG(0x783F);

    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_LAT_REG, 0x05);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_PWD_REG, 0x0C);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_PGS_REG, 0x02);
    N64_HW_WR(N64_PI_BASE_REG, PI_BSD_DOM2_RLS_REG, 0x02);
    /* MAGENTA = DOM2 timing done — all PI init complete */
    DIAG(0xF83F);

    N64_InitSP();
    /* WHITE = InitSP done */
    DIAG(0xFFFF);

    N64_InitVI();
    /* GREEN = InitVI done; fill both framebuffers */
    DIAG(0x07C1);
    {
        volatile u16 *fb1 = (volatile u16*)((uintptr_t)__fb1_start | 0x20000000u);
        for (int i = 0; i < 320 * 240; i++) fb1[i] = 0x07C1;
    }

    RDP_Init();

    N64_InitAI();
    /* BLUE = InitAI done */
    DIAG(0x003F);

    N64_InitInput();
    /* CYAN = InitInput done */
    DIAG(0x07FF);

    N64_InitFlashRAM();  /* no-op: sFlashRAMPresent = 0 */

    /* TEAL = about to enable interrupts (VI/SI are unmasked but IE still 0)
     * Changed from RED (0xF801) so it's distinct from first RED after MI_INTR_MASK.
     * TEAL = 0x07E1 = R:0 G:31 B:16 A:1 in RGBA5551                       */
    DIAG(0x07E1u);

    N64_EnableCPUInterrupts();
    /* YELLOW = interrupts enabled + first handler returned cleanly */
    DIAG(0xFFC1);

    {
        u32 count;
        asm volatile ("mfc0 %0, $9" : "=r"(count));
        extern u8 gN64IoRegs[];
        *(u16 *)(gN64IoRegs + 0x104) = (u16)(count ^ (count >> 16));
    }

    AgbMain();

    /* Should never reach here */
    for (;;)
        ;
}

/* -----------------------------------------------------------------------
 * N64_DmaSet — called by the DmaSet macro in macro.h
 *
 * For DMA channels 0-2 (used by M4A and the graphics engine for small
 * immediate transfers), execute synchronously via memcpy/memset.
 * For DMA channel 3 (the game's async VRAM DMA manager), store the
 * request in a pending-request queue and process it during VBlank.
 * --------------------------------------------------------------------- */

#define DMA3_QUEUE_SIZE 64

typedef struct {
    const void *src;
    void       *dst;
    u32         control;
    u32         fixedValue;  /* captured at queue time for DMA_SRC_FIXED (fill) requests */
} Dma3Request;

static Dma3Request sDma3Queue[DMA3_QUEUE_SIZE];
volatile int sDma3Head = 0;
volatile int sDma3Tail = 0;

void N64_DmaSet(int dmaNum, const void *src, void *dst, u32 control)
{
    u32 ctrl   = control >> 16;
    u32 count  = control & 0xFFFF;
    int is32   = (ctrl & DMA_32BIT) != 0;
    int fixed  = (ctrl & DMA_SRC_FIXED) != 0;
    u32 bytes  = count * (is32 ? 4 : 2);

    /* DMA3 runs immediately, exactly like DMA0-2 and like the real
     * hardware: DmaSet() stalls the CPU until the transfer completes.
     *
     * This used to queue the request for the next VBlank, which inverted
     * the order of two writes that the game expects to happen in sequence.
     * InitMainMenu() clears all of VRAM with DmaFill16(3, ...) and then
     * loads the window frame tiles with RequestDma3Copy(), which this port
     * runs immediately -- so the deferred clear landed afterwards and wiped
     * the tiles it was supposed to precede. The main menu's rounded window
     * borders were simply missing as a result.
     */

    /* Immediate execution for DMA0-2 */
    if (!(ctrl & DMA_ENABLE))
        return;

    if (fixed) {
        if (is32) {
            u32 val = *(const u32 *)src;
            u32 *d = (u32 *)dst;
            for (u32 i = 0; i < count; i++) *d++ = val;
        } else {
            u16 val = *(const u16 *)src;
            u16 *d = (u16 *)dst;
            for (u32 i = 0; i < count; i++) *d++ = val;
        }
    } else {
        memcpy(dst, src, bytes);
    }
}

/* ProcessDma3Requests — called from VBlank handler (replaces ProcessDma3Requests
 * in src/dma3_manager.c; the original is excluded from the N64 build) */
void ProcessDma3Requests(void)
{
    while (sDma3Tail != sDma3Head) {
        Dma3Request *req = &sDma3Queue[sDma3Tail];
        u32 ctrl  = req->control >> 16;
        u32 count = req->control & 0xFFFF;
        int is32  = (ctrl & DMA_32BIT) != 0;
        int fixed = (ctrl & DMA_SRC_FIXED) != 0;
        u32 bytes = count * (is32 ? 4 : 2);

        if (ctrl & DMA_ENABLE) {
            if (fixed) {
                if (is32) {
                    u32 val = req->fixedValue;
                    u32 *d = (u32 *)req->dst;
                    for (u32 i = 0; i < count; i++) *d++ = val;
                } else {
                    u16 val = (u16)req->fixedValue;
                    u16 *d = (u16 *)req->dst;
                    for (u32 i = 0; i < count; i++) *d++ = val;
                }
            } else {
                memcpy(req->dst, req->src, bytes);
            }
        }

        sDma3Tail = (sDma3Tail + 1) % DMA3_QUEUE_SIZE;
    }
}

/* -----------------------------------------------------------------------
 * Stubs for PI / AI done callbacks referenced in interrupt.c
 * (full implementations are in flashram.c and audio.c)
 * --------------------------------------------------------------------- */
__attribute__((weak)) void N64_PiDmaDone(void) {}
