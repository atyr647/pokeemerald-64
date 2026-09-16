/*
 * tools/ipl3.s — minimal N64 IPL3 for emulators that don't need RDRAM init
 *
 * This is the IPL3 for the `*.emu.z64` build. It is NOT for real hardware.
 *
 * Real IPL3 has to bring RDRAM up itself: assign each chip a device ID,
 * enable it, and run current-control calibration. libdragon's IPL3 does all
 * of that, which is why the hardware ROM uses it and why this file must not
 * replace it there — on a console (or on an accurate emulator like ares)
 * every RDRAM access before chip enable is dropped, so the copy below would
 * land nowhere and the jump would reach zeroed memory.
 *
 * The HLE emulators are the mirror image. In mupen64plus and the cores
 * derived from it, RDRAM is a plain host buffer that works from power-on and
 * needs no initialisation at all — while its RDRAM *register* emulation is
 * pattern-matched to Nintendo's IPL3 specifically. Two hacks in
 * device/rdram/rdram.c drive that: it deliberately serves corrupt RDRAM
 * between a broadcast write to RDRAM_DELAY and the next broadcast write to
 * RDRAM_MODE (bracketing what it assumes is current calibration), and at the
 * end it reads register s4 because "in the IPL3 procedure, at this point, the
 * amount of detected memory can be found in s4". libdragon's IPL3 is
 * different code that keeps different things in s4, so mupen64plus reads a
 * nonsense size out of it (64 MB), and the boot does not survive.
 *
 * Doing nothing is therefore the correct RDRAM procedure for those emulators,
 * and this stub does exactly that: copy the boot section out of the cartridge
 * and jump to it.
 *
 * The three constants come from the linked ELF and are supplied by
 * tools/patch_ipl3.py via --defsym, so this stub follows the linker script
 * instead of hardcoding a layout that silently goes stale:
 *
 *   IPL3_BOOT_LMA   cartridge address of .boot, KSEG1   (e.g. 0xB0001000)
 *   IPL3_BOOT_VMA   where .boot is linked to run        (e.g. 0x80360000)
 *   IPL3_BOOT_SIZE  size of .boot, rounded to 32 bytes  (e.g. 0x640)
 *
 * crt0.s (__n64_boot) takes over from there and copies .text, .data and the
 * read-only blobs into RDRAM itself, so only .boot travels here.
 *
 * The PIF copies bytes 0x040–0x0FFF of the cartridge into RSP IMEM and runs
 * it, so this must assemble to at most 4032 bytes. It is a few dozen.
 *
 * Register use (no ABI constraints at this point):
 *   $t0  cart source pointer, KSEG1 uncached
 *   $t1  RDRAM destination pointer, KSEG1 uncached
 *   $t2  end of destination range
 *   $t3  word scratch
 */

    .section .text
    .set    noreorder
    .globl  _start
    .align  2

_start:
    /* ---------------------------------------------------------------
     * Copy .boot from the cartridge into RDRAM.
     *
     * Both pointers are KSEG1 (uncached): writing straight through to
     * RDRAM means we do not depend on the data cache being in any
     * particular state this early, and there is no writeback to get
     * wrong afterwards.
     * --------------------------------------------------------------- */
    li      $t0, IPL3_BOOT_LMA              /* cart source, KSEG1        */
    li      $t1, (IPL3_BOOT_VMA & ~0xE0000000) | 0xA0000000  /* dest, KSEG1 */
    addiu   $t2, $t1, IPL3_BOOT_SIZE        /* end of destination        */

.Lcopy:
    lw      $t3, 0($t0)
    addiu   $t0, $t0, 4
    sw      $t3, 0($t1)
    addiu   $t1, $t1, 4
    bne     $t1, $t2, .Lcopy
    nop

    /* ---------------------------------------------------------------
     * Invalidate the instruction cache over the region we just wrote,
     * so the first fetch comes from RDRAM rather than a stale line.
     * CACHE 0x10 = Hit_Invalidate_I. Harmless where caches are not
     * modelled.
     * --------------------------------------------------------------- */
    li      $t1, IPL3_BOOT_VMA              /* KSEG0 for the cache ops   */
    addiu   $t2, $t1, IPL3_BOOT_SIZE

.Licache:
    cache   0x10, 0($t1)
    addiu   $t1, $t1, 32
    bne     $t1, $t2, .Licache
    nop

    /* ---------------------------------------------------------------
     * Enter __n64_boot at its link address.
     * --------------------------------------------------------------- */
    li      $t0, IPL3_BOOT_VMA
    jr      $t0
    nop
