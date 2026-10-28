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
    /* uint8_t direct address of the SFR supplying the high address byte of
       MOVX @Ri. Defaults to P2 (0xa0) like the classic 8051; derivatives
       with a dedicated page register (e.g. CC2530 MPAGE, 0x93) set it once.
       Kept across resets; values below 0x80 are rejected. */
    UC_MCS51_REG_XDATA_PAGE_SFR,
    /* uint8_t, 1 while an interrupt service routine runs (between the vector
       entry and RETI), i.e. UC_MCS51_REG_INTERRUPT_LEVELS != 0. Writing 0
       clears every in-progress level, so an SoC reset returns the core to
       its power-on interrupt state; writing nonzero marks level 0 in
       progress when no level is. */
    UC_MCS51_REG_INTERRUPT_IN_SERVICE,
    /* uint8_t, 1 (default): the core itself raises the classic 8051 Timer 0
       interrupt (IE.7 and IE.1 set, TCON.5 set -> vector 0x000b, TCON.5
       cleared) ahead of UC_MCS51_REG_IRQ_VECTOR. 0: the SoC model delivers
       every interrupt through UC_MCS51_REG_IRQ_VECTOR, e.g. when IE.1/TCON.5
       belong to another source with its own priority (CC2530 ADC). Kept
       across resets. */
    UC_MCS51_REG_TIMER0_INTERRUPT,
    /* uint8_t 0..7, priority level of UC_MCS51_REG_IRQ_VECTOR; post it before
       the vector. The core takes the posted vector at an instruction
       boundary when no level is in progress or the posted level is higher
       than every level in progress, then marks it in progress; RETI ends the
       highest level in progress. With every request at level 0 (the
       default) this is the classic single in-service latch. Reset to 0. */
    UC_MCS51_REG_IRQ_LEVEL,
    /* uint8_t bit mask, bit L set while an interrupt of level L is in
       progress (entry to RETI). Reset to 0. */
    UC_MCS51_REG_INTERRUPT_LEVELS,
    /* uint8_t, 0 (default): classic engine behaviour. 1: after RETI exactly
       one more instruction executes before any interrupt is taken (8051 RETI
       definition). Kept across resets. */
    UC_MCS51_REG_POST_RETI_RULE,
    /* uint8_t, 1 between a RETI (with UC_MCS51_REG_POST_RETI_RULE set) and
       the end of the following instruction's boundary check. Reset to 0. */
    UC_MCS51_REG_INTERRUPT_INHIBIT,
    /* uint8_t direct addresses of a second data pointer (e.g. CC2530 DPS
       0x92, DPL1 0x84, DPH1 0x85). With all three set (each >= 0x80), every
       DPTR instruction (MOV DPTR,#; INC DPTR; MOVX @DPTR; MOVC @A+DPTR;
       JMP @A+DPTR) uses DPL1/DPH1 while bit 0 of the DPS SFR is 1. 0 (the
       default) disables the second pointer. Kept across resets.
       UC_MCS51_REG_DPL/DPH/DPTR always address DPL/DPH (0x82/0x83). */
    UC_MCS51_REG_DPS_SFR,
    UC_MCS51_REG_DPL1_SFR,
    UC_MCS51_REG_DPH1_SFR,
    /* uint8_t, read-only: 1 while the core reads the destination operand of
       a read-modify-write instruction (ANL/ORL/XRL direct, INC/DEC direct,
       DJNZ direct, JBC, CPL/CLR/SETB bit, MOV bit,C), 0 otherwise. An SFR
       read callback can use it to return a port latch instead of the pins,
       as the 8051 does for these instructions. */
    UC_MCS51_REG_RMW_ACCESS,
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
