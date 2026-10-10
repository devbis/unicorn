#include "qemu/osdep.h"
#include "cpu.h"
#include "uc_priv.h"
#include "exec/exec-all.h"

hwaddr tc32_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr)
{
    (void)cpu;
    return addr;
}

bool tc32_cpu_tlb_fill(CPUState *cpu, vaddr address, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool probe, uintptr_t retaddr)
{
    (void)size;
    (void)access_type;
    (void)probe;
    (void)retaddr;
    tlb_set_page(cpu, address & TARGET_PAGE_MASK, address & TARGET_PAGE_MASK,
                 PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
                 TARGET_PAGE_SIZE);
    return true;
}

void helper_uc_tc32_exit(TC32CPUState *env)
{
    CPUState *cpu = env->uc->cpu;
    cpu->exception_index = EXCP_HLT;
    cpu->halted = 1;
    cpu_loop_exit(cpu);
}

void helper_uc_tc32_invalid(TC32CPUState *env)
{
    CPUState *cpu = env->uc->cpu;
    env->uc->invalid_error = UC_ERR_INSN_INVALID;
    cpu->exception_index = EXCP_HLT;
    cpu->halted = 1;
    cpu_loop_exit(cpu);
}

void helper_uc_tc32_write_cpsr(TC32CPUState *env, uint32_t value)
{
    tc32_write_cpsr(env, value);
}

void helper_uc_tc32_treti(TC32CPUState *env)
{
    unsigned mode = env->cpsr & TC32_CPSR_MODE_MASK;
    uint32_t target = env->regs[14];

    tc32_write_cpsr(env, env->spsr[mode]);
    env->regs[15] = target & ~1u;
}
