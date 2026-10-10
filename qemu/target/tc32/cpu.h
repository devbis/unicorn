#ifndef TC32_CPU_H
#define TC32_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-defs.h"

#define TARGET_INSN_START_EXTRA_WORDS 1
#define TC32_CPSR_MODE_MASK 0x1fu
#define TC32_CPSR_MODE_IRQ  0x12u
#define TC32_CPSR_MODE_SVC  0x13u

typedef struct TC32CPUState {
    uint32_t regs[16];
    uint32_t cpsr;
    uint32_t ssr;
    uint32_t banked_sp[32];
    uint32_t banked_lr[32];
    uint32_t spsr[32];

    int end_reset_fields;
    uint64_t fetch_alias_base;
    uint64_t fetch_alias_size;
    uint64_t fetch_alias_source;
    struct uc_struct *uc;
} TC32CPUState;

typedef TC32CPUState CPUArchState;
typedef TC32CPU ArchCPU;

struct TC32CPU {
    CPUState parent_obj;
    CPUNegativeOffsetState neg;
    TC32CPUState env;
    TC32CPUClass cc;
};

void tc32_tcg_init(struct uc_struct *uc);
TC32CPU *cpu_tc32_init(struct uc_struct *uc);
void tc32_write_cpsr(TC32CPUState *env, uint32_t value);
hwaddr tc32_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr);
bool tc32_cpu_tlb_fill(CPUState *cpu, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr);

static inline int cpu_mmu_index(TC32CPUState *env, bool ifetch)
{
    (void)env;
    (void)ifetch;
    return 0;
}

static inline void cpu_get_tb_cpu_state(TC32CPUState *env, target_ulong *pc,
                                        target_ulong *cs_base, uint32_t *flags)
{
    *pc = env->regs[15];
    *cs_base = 0;
    *flags = 0;
}

#include "exec/cpu-all.h"

#endif
