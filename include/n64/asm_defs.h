/*
 * include/n64/asm_defs.h
 *
 * Common assembler definitions for N64 MIPS assembly files.
 * Included by crt0.s and any other .s files in src/n64/.
 */

#ifndef GUARD_N64_ASM_DEFS_H
#define GUARD_N64_ASM_DEFS_H

/* N64 hardware register base addresses (physical) */
#define N64_MI_BASE_REG     0xA4300000
#define N64_VI_BASE_REG     0xA4400000
#define N64_AI_BASE_REG     0xA4500000
#define N64_PI_BASE_REG     0xA4600000
#define N64_RI_BASE_REG     0xA4700000
#define N64_SI_BASE_REG     0xA4800000
#define N64_SP_BASE_REG     0xA4040000
#define N64_DP_BASE_REG     0xA4100000

/* MI interrupt register offsets */
#define MI_INTR_MASK_REG    0x0C
#define MI_INTR_REG         0x08
#define MI_MODE_REG         0x00
#define MI_VERSION_REG      0x04

/* VI register offsets */
#define VI_STATUS_REG       0x00
#define VI_ORIGIN_REG       0x04
#define VI_WIDTH_REG        0x08
#define VI_INTR_REG         0x0C
#define VI_CURRENT_REG      0x10
#define VI_BURST_REG        0x14
#define VI_V_SYNC_REG       0x18
#define VI_H_SYNC_REG       0x1C
#define VI_LEAP_REG         0x20
#define VI_H_START_REG      0x24
#define VI_V_START_REG      0x28
#define VI_V_BURST_REG      0x2C
#define VI_X_SCALE_REG      0x30
#define VI_Y_SCALE_REG      0x34

/* AI register offsets */
#define AI_DRAM_ADDR_REG    0x00
#define AI_LEN_REG          0x04
#define AI_CONTROL_REG      0x08
#define AI_STATUS_REG       0x0C
#define AI_DACRATE_REG      0x10
#define AI_BITRATE_REG      0x14

/* PI register offsets */
#define PI_DRAM_ADDR_REG    0x00
#define PI_CART_ADDR_REG    0x04
#define PI_RD_LEN_REG       0x08
#define PI_WR_LEN_REG       0x0C
#define PI_STATUS_REG       0x10
#define PI_BSD_DOM1_LAT_REG 0x14
#define PI_BSD_DOM1_PWD_REG 0x18
#define PI_BSD_DOM1_PGS_REG 0x1C
#define PI_BSD_DOM1_RLS_REG 0x20
#define PI_BSD_DOM2_LAT_REG 0x24
#define PI_BSD_DOM2_PWD_REG 0x28
#define PI_BSD_DOM2_PGS_REG 0x2C
#define PI_BSD_DOM2_RLS_REG 0x30

/* SI register offsets */
#define SI_DRAM_ADDR_REG    0x00
#define SI_PIF_ADDR_RD64B   0x04
#define SI_PIF_ADDR_WR64B   0x10
#define SI_STATUS_REG       0x18

/* DPC (RDP command) register offsets */
#define DPC_START_REG       0x00
#define DPC_END_REG         0x04
#define DPC_CURRENT_REG     0x08
#define DPC_STATUS_REG      0x0C

/* CP0 register numbers */
#define CP0_INDEX       0
#define CP0_RANDOM      1
#define CP0_ENTRYLO0    2
#define CP0_ENTRYLO1    3
#define CP0_CONTEXT     4
#define CP0_PAGEMASK    5
#define CP0_WIRED       6
#define CP0_BADVADDR    8
#define CP0_COUNT       9
#define CP0_ENTRYHI     10
#define CP0_COMPARE     11
#define CP0_STATUS      12
#define CP0_CAUSE       13
#define CP0_EPC         14
#define CP0_PRID        15
#define CP0_CONFIG      16
#define CP0_LLADDR      17
#define CP0_WATCHLO     18
#define CP0_WATCHHI     19
#define CP0_XCONTEXT    20
#define CP0_TAGLO       28
#define CP0_TAGHI       29
#define CP0_ERROREPC    30

/* CP0 Status register bits */
#define SR_IE       0x00000001  /* Interrupt enable                        */
#define SR_EXL      0x00000002  /* Exception level                         */
#define SR_ERL      0x00000004  /* Error level                             */
#define SR_IM0      0x00000100  /* SW interrupt 0 mask                     */
#define SR_IM1      0x00000200  /* SW interrupt 1 mask                     */
#define SR_IMASK    0x0000FC00  /* HW interrupt mask (IP2-IP7)             */
#define SR_IM2      0x00000400  /* HW int 0 (RCP)                          */
#define SR_BEV      0x00400000  /* Boot exception vectors                  */
#define SR_CU0      0x10000000  /* CP0 usable                              */
#define SR_CU1      0x20000000  /* CP1 (FPU) usable                        */
#define SR_FR       0x04000000  /* 64-bit FPU registers                    */

/* CP0 Cause register bits */
#define CAUSE_EXCCODE   0x0000007C  /* Exception code                      */
#define CAUSE_IP        0x0000FF00  /* Interrupt pending                   */
#define CAUSE_IP2       0x00000400  /* HW interrupt 0 (RCP)                */
#define CAUSE_BD        0x80000000  /* Branch delay slot                   */

/* MI interrupt bits */
#define MI_INTR_SP      0x01    /* RSP interrupt                           */
#define MI_INTR_SI      0x02    /* Serial interface interrupt              */
#define MI_INTR_AI      0x04    /* Audio interface interrupt               */
#define MI_INTR_VI      0x08    /* Video interface interrupt               */
#define MI_INTR_PI      0x10    /* Peripheral interface interrupt          */
#define MI_INTR_DP      0x20    /* RDP interrupt                           */

/* -----------------------------------------------------------------------
 * MMIO access macros
 *
 * The N64 CPU is compiled in big-endian mode (-EB), matching the
 * hardware endianness of all N64 MMIO registers (VI, AI, PI, SI, MI,
 * SP, DP).  No byte-swapping is needed: a `lw` from a hardware register
 * returns the value directly, and `sw` writes it directly.
 * --------------------------------------------------------------------- */
#ifndef __ASSEMBLER__

#include <stdint.h>

/* Read a 32-bit hardware register */
static inline uint32_t n64_mmio_rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* Write a 32-bit value to a hardware register */
static inline void n64_mmio_wr32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

#define N64_HW_RD(base, off)        n64_mmio_rd32((uint32_t)((base) + (off)))
#define N64_HW_WR(base, off, val)   n64_mmio_wr32((uint32_t)((base) + (off)), (uint32_t)(val))

/* Convenience: N64 hardware register access with absolute address */
#define N64_REG_RD(addr)            n64_mmio_rd32((uint32_t)(addr))
#define N64_REG_WR(addr, val)       n64_mmio_wr32((uint32_t)(addr), (uint32_t)(val))

#endif /* !__ASSEMBLER__ */

#endif /* GUARD_N64_ASM_DEFS_H */
