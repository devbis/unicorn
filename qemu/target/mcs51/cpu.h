#ifndef MCS51_CPU_H
#define MCS51_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-defs.h"
#include "unicorn/mcs51.h"

typedef struct MCS51CPUState {
    uint32_t pc;
    uint64_t cycles;
    uint64_t instructions;
    uint16_t interrupt_vector;
    uint8_t interrupt_level;     /* level of interrupt_vector */
    uint8_t interrupt_levels;    /* bit L: level L in progress */
    uint8_t interrupt_inhibit;   /* post-RETI one-instruction inhibit */
    int end_reset_fields;
    struct uc_struct *uc;
    /* Host-provided fast path (see unicorn/mcs51.h); kept across resets. */
    uc_mcs51_fast_memory fast;
    uint64_t event_cycles;
    uint64_t instruction_limit;
    uint32_t break_pc;
    uint8_t xdata_page_sfr;
    uint8_t timer0_interrupt;
    uint8_t post_reti_rule;
    uint8_t dps_sfr;   /* 0: no second data pointer */
    uint8_t dpl1_sfr;
    uint8_t dph1_sfr;
    uint8_t rmw_access; /* reading a read-modify-write destination */
    uint8_t cycle_model; /* UC_MCS51_CYCLE_MODEL_* */
} MCS51CPUState;

typedef MCS51CPUState CPUArchState;
typedef MCS51CPU ArchCPU;

struct MCS51CPU {
    CPUState parent_obj;
    CPUNegativeOffsetState neg;
    MCS51CPUState env;
    MCS51CPUClass cc;
};

void mcs51_tcg_init(struct uc_struct *uc);
MCS51CPU *cpu_mcs51_init(struct uc_struct *uc);
hwaddr mcs51_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr);
bool mcs51_cpu_tlb_fill(CPUState *cpu, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr);

static inline int cpu_mmu_index(MCS51CPUState *env, bool ifetch)
{
    (void)env;
    (void)ifetch;
    return 0;
}

static inline void cpu_get_tb_cpu_state(MCS51CPUState *env, target_ulong *pc,
                                        target_ulong *cs_base, uint32_t *flags)
{
    *pc = env->pc & 0xffffu;
    *cs_base = 0;
    *flags = 0;
}

#include "exec/cpu-all.h"

#endif
