/*
 * n64.ld — Nintendo 64 linker script for pokeemerald-64
 *
 * N64 memory map:
 *   0x80000000 - 0x80400000  RDRAM (4MB standard)
 *   0x80400000 - 0x80800000  RDRAM expansion pak (4MB, optional)
 *   0xA0000000 - ...         Uncached mirrors of RDRAM
 *   0xB0000000 - 0xBFFFFFFF  ROM via PI bus (cart)
 *
 * ROM is accessed at 0xB0000000 (uncached) or 0x90000000 (cached).
 * The N64 boot process copies the first 1MB of ROM to RDRAM via PI DMA,
 * then jumps to the entry point in RDRAM.
 *
 * We use the libdragon convention:
 *   - Code/data live in RDRAM at 0x80000400 (after the 1KB exception vectors)
 *   - ROM image is built as a flat binary starting at offset 0x1000 (after
 *     the 4KB ROM header / IPL3 boot code region)
 */

OUTPUT_ARCH(mips)
ENTRY(__n64_boot)

MEMORY
{
    /* RDRAM — 4MB base + 4MB expansion pak = 8MB total.
     * Expansion pak REQUIRED (the game's buffers live at 0x80790000+).
     * .boot VMA = 0x80360000, loaded there by libdragon's compat IPL3.
     * .text / .data are copied to 0x80000400+ by crt0.s at boot.
     * Low 1KB (0x80000000-0x800003FF) is used for exception vectors.        */
    RDRAM  (rwx) : ORIGIN = 0x80000400, LENGTH = 8M - 0x400

    /* ROM image: the full cartridge space via KSEG1 uncached (0xB0000000+).
     * Sections with `AT > ROM` store their bytes here but execute/load from
     * their VMA (RDRAM for code/data, or ROM itself for read-only data).
     * The 4KB at 0xB0000000 is the N64 header; game sections start at +0x1000 */
    ROM    (rx)  : ORIGIN = 0xB0000000, LENGTH = 64M

    /* The slice of RDRAM holding the read-only data that has to be copied
     * out of cartridge ROM: most of .rodata plus all of .script_data.
     * Runs from above the boot stub to where the software GBA
     * palette/VRAM/OAM buffers begin (the stack lives at the very top of
     * that gap, growing down from __stack_top). Overflowing this is a link
     * error rather than a mysterious corruption at runtime.
     *
     * It used to start at a round 0x80380000, which left 128 KB between it
     * and the boot stub doing nothing; reclaiming that is what let the song
     * data in. There is another 170 KB below .boot, between it and the end
     * of .bss, if this runs out again -- taking that one means moving
     * .boot, whose VMA has to stay in step with the entry point in
     * src/n64/rom_header.s. */
    RDRAM2 (rw)  : ORIGIN = 0x80360800, LENGTH = 0x42F800
}

/*
 * Software-emulated GBA VRAM, OAM and Palette RAM.
 * These live in normal RDRAM — the software compositor reads them directly.
 * We keep them at fixed well-known addresses so that GBA-style pointer casts
 * (e.g. (u16*)PLTT) still compile; we simply redefine the constants to point
 * here instead of to GBA MMIO space.
 *
 * Layout (within the RDRAM segment, after BSS):
 *   PLTT_BUF  : 1024 bytes  (512 BG + 512 OBJ palette, RGB555)
 *   VRAM_BUF  : 98304 bytes (96KB tile + screen data)
 *   OAM_BUF   : 1024 bytes  (128 × 8-byte OAM entries)
 *   SW_IOREGS : 4096 bytes  (software-state I/O register file)
 */

SECTIONS
{
    /* ------------------------------------------------------------------ */
    /*  N64 ROM header — lives at ROM byte 0 (LMA = 0xB0000000).         */
    /*  VMA is also ROM since the header is never copied to RDRAM.        */
    /* ------------------------------------------------------------------ */
    .n64header 0xB0000000 :
    {
        KEEP(*(.n64header))
        KEEP(*(.n64header.*))
    } > ROM

    /* ------------------------------------------------------------------ */
    /*  Boot / entry stub — loaded by libdragon's compat IPL3.            */
    /*                                                                     */
    /*  IPL3 (boot/loader_compat.c) reads the entry point from ROM[0x08],  */
    /*  DMA's ROM[0x1000..] to that address, and jumps to it.  So this     */
    /*  VMA must equal the entry point in src/n64/rom_header.s, and this   */
    /*  must be the first section after the 0x1000-byte .n64header so its  */
    /*  LMA lands exactly at ROM[0x1000].                                  */
    /*                                                                     */
    /*  Placement constraints (see rom_header.s for the full reasoning):   */
    /*    - NOT in the top 32 KiB (0x807F8000+): libdragon reserves that   */
    /*      for its loader/stack and clears it before jumping.             */
    /*    - IPL3 falls back to DMA'ing 1 MiB from here, which lands in the */
    /*      RDRAM2 const-data window; harmless, crt0 fills that window      */
    /*      afterwards.                                                     */
    /*    - Below RDRAM2 (0x80380000), so the const-data copies below never */
    /*      overwrite the boot stub while it is still running.             */
    /*                                                                     */
    /*  crt0.s (__n64_boot) runs from 0x80360000, copies .text, .data,     */
    /*  .rodata and .script_data into RDRAM, then calls N64Main().         */
    /* ------------------------------------------------------------------ */
    .boot 0x80360000 :
    {
        KEEP(*(.boot))
        KEEP(*(.boot.*))
    } > RDRAM AT > ROM

    /* ------------------------------------------------------------------ */
    /*  Code and writable data: VMA in RDRAM, LMA in ROM (copied at boot) */
    /*  .text explicitly starts at 0x80000400 — crt0.s DMA's it here.    */
    /* ------------------------------------------------------------------ */
    .text 0x80000400 :
    {
        __text_start = .;
        *(.text.unlikely .text.*_unlikely .text.unlikely.*)
        *(.text.exit .text.exit.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
        *(.text .stub .text.* .gnu.linkonce.t.*)
        KEEP(*(.text.*))
        . = ALIGN(8);
        __text_end = .;
    } > RDRAM AT > ROM

    /* LMA of the text section (ROM address to copy from at boot) */
    __text_lma = LOADADDR(.text);

    /* ------------------------------------------------------------------ */
    /*  Bulk read-only blobs: stay in ROM, read over the KSEG1 PI bus.    */
    /*  VMA = LMA = ROM address (0xB0001000+), no RDRAM copy needed.      */
    /*                                                                     */
    /*  The PI bus only answers WORD-sized reads. A byte or halfword load  */
    /*  from cartridge space is turned into a word read at the unaligned   */
    /*  address, and the PI rounds that down to a halfword boundary, so a  */
    /*  byte at word-offset 2 or 3 comes back holding the byte from two    */
    /*  bytes further on. That is real N64 behaviour, not an emulator      */
    /*  quirk, and it means anything the game reads through a u8* or u16*  */
    /*  cannot live here -- every text string and struct table included.   */
    /*                                                                     */
    /*  All of .rodata is 6.3 MB and does not fit in RDRAM, but the parts  */
    /*  that do not fit are exactly the parts nothing reads that way: tile */
    /*  graphics, palettes, font glyph bitmaps, sound banks and multiboot  */
    /*  images, which all arrive by DMA or through the decompressors, and  */
    /*  those read words. tools/n64_rodata_split.py picks them out by      */
    /*  symbol name and size and writes the list included here; everything */
    /*  it does not name falls through to the RDRAM .rodata below.        */
    /* ------------------------------------------------------------------ */
    .rodata.blobs :
    {
        INCLUDE build/n64/rodata_rom.ld
        . = ALIGN(8);
    } > ROM

    /* GBA ROM data (tile graphics, maps, scripts etc.) */
    .rodata.gba :
    {
        *(.rodata.gba .rodata.gba.*)
        . = ALIGN(8);
    } > ROM

    /* GBA script/event/text data from data/*.s -- every message in the
     * game plus the event scripts. Copied to RDRAM with .rodata below: the
     * script engine and all the string routines walk it a byte at a time,
     * which cartridge ROM cannot serve correctly.
     * NOTE: input sections are named "script_data" (no leading dot),
     * so we capture both forms: with and without the leading dot.      */
    .script_data :
    {
        __script_data_start = .;
        *(script_data script_data.*)
        *(.script_data .script_data.*)
        . = ALIGN(8);
        __script_data_end = .;
    } > RDRAM2 AT > ROM

    __script_data_lma = LOADADDR(.script_data);

    /* ------------------------------------------------------------------ */
    /*  The rest of .rodata: copied to RDRAM at boot by crt0.s so that     */
    /*  byte and halfword reads of it behave (see .rodata.blobs above).    */
    /*  It sits in RDRAM2, the window between the 1 MB IPL3 loads at       */
    /*  0x80400000 and the software GBA buffers at 0x80790000.            */
    /* ------------------------------------------------------------------ */
    .rodata :
    {
        __rodata_start = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        . = ALIGN(8);
        __rodata_end = .;
    } > RDRAM2 AT > ROM

    __rodata_lma = LOADADDR(.rodata);

    /* ------------------------------------------------------------------ */
    /*  Initialised data                                                   */
    /* ------------------------------------------------------------------ */
    .data :
    {
        __data_start = .;
        *(.data .data.* .gnu.linkonce.d.*)
        . = ALIGN(8);
    } > RDRAM AT > ROM

    /* LMA of the first data section (ROM address to copy from at boot)   */
    __data_lma = LOADADDR(.data);

    /* GBA "EWRAM data" → maps to normal RDRAM                            */
    .ewram_data :
    {
        *(.ewram_data)
        . = ALIGN(8);
    } > RDRAM AT > ROM

    /* GBA "IWRAM data" → also maps to RDRAM (no speed penalty on N64)    */
    .iwram_data :
    {
        *(.iwram_data)
        . = ALIGN(8);
    } > RDRAM AT > ROM

    /* GBA "common data" section                                           */
    .common_data :
    {
        *(.common_data)
        . = ALIGN(8);
        __data_end = .;
    } > RDRAM AT > ROM

    /* ------------------------------------------------------------------ */
    /*  BSS (zero-initialised)                                             */
    /* ------------------------------------------------------------------ */
    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RDRAM

    /* ------------------------------------------------------------------ */
    /*  Scratch page that the low 32 KB of user space is mapped onto, so   */
    /*  that a stray read or write through a NULL pointer lands somewhere  */
    /*  harmless instead of taking a TLB miss. See crt0.s.                 */
    /* ------------------------------------------------------------------ */
    .nullpage (NOLOAD) :
    {
        . = ALIGN(0x4000);
        __null_page = .;
        . += 0x4000;
    } > RDRAM

    /* ------------------------------------------------------------------ */
    /*  Software GBA hardware buffers (all NOLOAD — zeroed at boot)        */
    /* ------------------------------------------------------------------ */

    /* Software buffers at fixed addresses so BG_VRAM/PLTT are compile-time constants */
    /* ------------------------------------------------------------------ */
    /*  Heap (grows upward from BSS end; bounded by stack bottom)         */
    /* ------------------------------------------------------------------ */
    .heap (NOLOAD) :
    {
        __heap_start = .;
        . = ALIGN(8);
    } > RDRAM

    /* ------------------------------------------------------------------ */
    /*  Upper RDRAM fixed layout (expansion pak required for 8MB).        */
    /*                                                                     */
    /*  0x807B0000 - 0x807FB000: Double-buffered N64 framebuffers (2×150KB)*/
    /*  0x807B0000 - 0x807D5800: FB0 (320×240 RGBA5551)                  */
    /*  0x807D6000 - 0x807FB800: FB1 (320×240 RGBA5551)                  */
    /*                                                                     */
    /*  0x80790000 - 0x807AC000: Software GBA hardware buffers           */
    /*  0x80790000: PLTT  (1024 bytes)                                    */
    /*  0x80791000: VRAM  (96KB = 0x18000)                                */
    /*  0x807A9000: OAM   (1024 bytes)                                    */
    /*  0x807A9400: IOREGS(4096 bytes)                                    */
    /* ------------------------------------------------------------------ */

    .sw_palette 0x80790000 (NOLOAD) :
    {
        __sw_palette_start = .;
        . += 0x400;   /* 1024 bytes = 512 BG + 512 OBJ palette entries    */
        __sw_palette_end = .;
    } > RDRAM

    .sw_vram 0x80791000 (NOLOAD) :
    {
        __sw_vram_start = .;
        . += 0x18000; /* 96KB  = BG tile + screen data + OBJ tile area    */
        __sw_vram_end = .;
    } > RDRAM

    .sw_oam 0x807A9000 (NOLOAD) :
    {
        __sw_oam_start = .;
        . += 0x400;   /* 1024 bytes = 128 sprites × 8 bytes               */
        __sw_oam_end = .;
    } > RDRAM

    .sw_ioregs 0x807A9400 (NOLOAD) :
    {
        __sw_ioregs_start = .;
        . += 0x1000;  /* 4096 bytes of software I/O register state         */
        __sw_ioregs_end = .;
    } > RDRAM

    /* N64 scaled-up framebuffers (320×240, 2 bytes/pixel = 153600 bytes each) */
    .framebuffers 0x807B0000 (NOLOAD) :
    {
        . = ALIGN(64);
        __fb0_start = .;
        . += 320 * 240 * 2;   /* framebuffer 0                            */
        . = ALIGN(64);
        __fb1_start = .;
        . += 320 * 240 * 2;   /* framebuffer 1                            */
        . = ALIGN(64);
    } > RDRAM

    /* ------------------------------------------------------------------ */
    /*  Symbols for the runtime                                            */
    /* ------------------------------------------------------------------ */
    __rdram_start    = ORIGIN(RDRAM);
    __rdram_size     = LENGTH(RDRAM);
    __rom_start      = ORIGIN(ROM);

    /* _gp: MIPS global pointer — unused (G0 means no GP-relative addressing)
     * but crt0 loads it; define it as 0 to satisfy the reference. */
    PROVIDE(_gp = 0);

    /* Stack — 64KB, placed just below SW buffers at 0x80790000           */
    __stack_top      = 0x80790000;
    __stack_size     = 0x10000;   /* 64KB                                 */
    __stack_bottom   = __stack_top - __stack_size;

    /DISCARD/ :
    {
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.pdr)
        *(.mdebug*)
        *(.comment)
        *(.note.*)
    }
}
