/* This file is released under LGPL2.
   See COPYING.LGPL2 in root directory for more details
*/

#ifndef UNICORN_TC32_H
#define UNICORN_TC32_H

#ifdef __cplusplus
extern "C" {
#endif

//> TC32 registers
typedef enum uc_tc32_reg {
    UC_TC32_REG_INVALID = 0,
    UC_TC32_REG_R0,
    UC_TC32_REG_R1,
    UC_TC32_REG_R2,
    UC_TC32_REG_R3,
    UC_TC32_REG_R4,
    UC_TC32_REG_R5,
    UC_TC32_REG_R6,
    UC_TC32_REG_R7,
    UC_TC32_REG_R8,
    UC_TC32_REG_R9,
    UC_TC32_REG_R10,
    UC_TC32_REG_R11,
    UC_TC32_REG_R12,
    UC_TC32_REG_R13,
    UC_TC32_REG_R14,
    UC_TC32_REG_R15,
    UC_TC32_REG_CPSR,
    UC_TC32_REG_SPSR,
    UC_TC32_REG_SSR,
    UC_TC32_REG_ENDING,

    UC_TC32_REG_SP = UC_TC32_REG_R13,
    UC_TC32_REG_LR = UC_TC32_REG_R14,
    UC_TC32_REG_PC = UC_TC32_REG_R15,
} uc_tc32_reg;

#define UC_TC32_FLAG_N (1u << 31)
#define UC_TC32_FLAG_Z (1u << 30)
#define UC_TC32_FLAG_C (1u << 29)
#define UC_TC32_FLAG_V (1u << 28)

#ifdef __cplusplus
}
#endif

#endif
