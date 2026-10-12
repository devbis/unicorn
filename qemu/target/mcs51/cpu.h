#ifndef MCS51_CPU_H
#define MCS51_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-defs.h"

typedef struct MCS51CPUState {
    uint32_t pc;
    uint64_t cycles;
    uint64_t instructions;
    uint16_t interrupt_vector;
    uint8_t interrupt_in_service;
    int end_reset_fields;
    struct uc_struct *uc;
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
