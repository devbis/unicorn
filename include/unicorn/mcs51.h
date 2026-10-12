/* This file is released under LGPL2.
   See COPYING.LGPL2 in root directory for more details
*/

#ifndef UNICORN_MCS51_H
#define UNICORN_MCS51_H

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
    UC_MCS51_REG_ENDING,
} uc_mcs51_reg;

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
