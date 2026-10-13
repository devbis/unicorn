/* This file is released under LGPL2.
   See COPYING.LGPL2 in root directory for more details
*/

#ifndef UNICORN_MCS51_H
#define UNICORN_MCS51_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MCS-51 virtual memory spaces used by the Unicorn backend. */
#define UC_MCS51_XDATA_BASE 0x00010000ULL
#define UC_MCS51_IDATA_BASE 0x00020000ULL
#define UC_MCS51_SFR_BASE   0x00030000ULL

typedef enum uc_mcs51_reg {
    UC_MCS51_REG_INVALID = 0,
    UC_MCS51_REG_A,
    UC_MCS51_REG_B,
    UC_MCS51_REG_PSW,
    UC_MCS51_REG_SP,
    UC_MCS51_REG_DPL,
    UC_MCS51_REG_DPH,
    UC_MCS51_REG_DPTR,
    UC_MCS51_REG_R0,
    UC_MCS51_REG_R1,
    UC_MCS51_REG_R2,
    UC_MCS51_REG_R3,
    UC_MCS51_REG_R4,
    UC_MCS51_REG_R5,
    UC_MCS51_REG_R6,
    UC_MCS51_REG_R7,
    UC_MCS51_REG_PC,
    UC_MCS51_REG_CYCLES,
    UC_MCS51_REG_IRQ_VECTOR,
    UC_MCS51_REG_INSTRUCTIONS,
    /* Host-side fast path; see uc_mcs51_fast_memory. */
    UC_MCS51_REG_FAST_MEMORY,   /* const uc_mcs51_fast_memory *, copied in */
    UC_MCS51_REG_EVENT_CYCLES,  /* uint64_t */
    UC_MCS51_REG_INSTRUCTION_LIMIT, /* uint64_t, absolute instruction count */
    UC_MCS51_REG_BREAK_PC,      /* uint32_t, >0xffff disables */
    UC_MCS51_REG_ENDING,
} uc_mcs51_reg;

/*
 * Direct access to the memories of the model, so the interpreter does not go
 * through the soft MMU for every register access. The arrays must be the very
 * memory the corresponding address spaces are mapped onto (uc_mem_map_ptr), and
 * stay valid while the engine lives.
 *
 *   code    64 KiB of program memory
 *   idata   256 bytes, the internal RAM at UC_MCS51_IDATA_BASE
 *   sfr     256 bytes indexed by the direct address (0x80..0xff are used)
 *
 * An SFR flagged in sfr_special_read / sfr_special_write (256 flags each) still
 * goes through the mapped SFR region, so a host callback sees the access. All
 * other SFRs are plain memory.
 *
 * With this set the interpreter also runs many instructions per translated
 * block (when no code hook is installed) and returns to the host, via
 * uc_emu_stop semantics, when one of these holds before an instruction:
 *   cycles >= UC_MCS51_REG_EVENT_CYCLES
 *   instructions >= UC_MCS51_REG_INSTRUCTION_LIMIT
 *   pc == UC_MCS51_REG_BREAK_PC
 * At least one instruction always runs per uc_emu_start.
 */
typedef struct uc_mcs51_fast_memory {
    uint8_t *code;
    uint8_t *idata;
    uint8_t *sfr;
    const uint8_t *sfr_special_read;
    const uint8_t *sfr_special_write;
} uc_mcs51_fast_memory;

#define UC_MCS51_FLAG_CY 0x80u
#define UC_MCS51_FLAG_AC 0x40u
#define UC_MCS51_FLAG_F0 0x20u
#define UC_MCS51_FLAG_RS1 0x10u
#define UC_MCS51_FLAG_RS0 0x08u
#define UC_MCS51_FLAG_OV 0x04u
#define UC_MCS51_FLAG_F1 0x02u
#define UC_MCS51_FLAG_P 0x01u

#ifdef __cplusplus
}
#endif

#endif
