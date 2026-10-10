#include "qemu/osdep.h"
#include "cpu.h"
#include "uc_priv.h"
#include "exec/exec-all.h"

static void tc32_cpu_set_pc(CPUState *cpu, vaddr value)
{
    TC32CPU *tc32 = TC32_CPU(cpu);
    tc32->env.regs[15] = value & ~(target_ulong)1;
}

static void tc32_cpu_synchronize_from_tb(CPUState *cpu, TranslationBlock *tb)
{
    TC32_CPU(cpu)->env.regs[15] = tb->pc;
}

static void tc32_cpu_reset(CPUState *cpu)
{
    TC32CPU *tc32 = TC32_CPU(cpu);
    TC32CPUClass *tcc = TC32_CPU_GET_CLASS(tc32);

    tcc->parent_reset(cpu);
    memset(&tc32->env, 0, offsetof(TC32CPUState, end_reset_fields));
    tc32->env.cpsr = TC32_CPSR_MODE_SVC;
}

static bool tc32_cpu_has_work(CPUState *cpu)
{
    (void)cpu;
    return true;
}

static void tc32_cpu_initfn(struct uc_struct *uc, CPUState *cpu)
{
    TC32CPU *tc32 = TC32_CPU(cpu);
    tc32->env.uc = uc;
    cpu_set_cpustate_pointers(tc32);
}

static void tc32_cpu_class_init(CPUClass *cc)
{
    TC32CPUClass *tcc = TC32_CPU_CLASS(cc);

    tcc->parent_reset = cc->reset;
    cc->reset = tc32_cpu_reset;
    cc->has_work = tc32_cpu_has_work;
    cc->set_pc = tc32_cpu_set_pc;
    cc->synchronize_from_tb = tc32_cpu_synchronize_from_tb;
    cc->get_phys_page_debug = tc32_cpu_get_phys_page_debug;
    cc->tlb_fill_cpu = tc32_cpu_tlb_fill;
    cc->tcg_initialize = tc32_tcg_init;
}

TC32CPU *cpu_tc32_init(struct uc_struct *uc)
{
    TC32CPU *tc32 = qemu_memalign(8, sizeof(*tc32));
    CPUState *cpu;
    CPUClass *cc;

    if (tc32 == NULL) {
        return NULL;
    }
    memset(tc32, 0, sizeof(*tc32));
    cpu = (CPUState *)tc32;
    cc = (CPUClass *)&tc32->cc;
    cpu->cc = cc;
    cpu->uc = uc;
    uc->cpu = cpu;

    cpu_class_init(uc, cc);
    tc32_cpu_class_init(cc);
    cpu_common_initfn(uc, cpu);
    tc32_cpu_initfn(uc, cpu);
    cpu_exec_realizefn(cpu);
    cpu_reset(cpu);
    cpu_address_space_init(cpu, 0, cpu->memory);
    qemu_init_vcpu(cpu);
    return tc32;
}

void tc32_write_cpsr(TC32CPUState *env, uint32_t value)
{
    unsigned old_mode = env->cpsr & TC32_CPSR_MODE_MASK;
    unsigned new_mode = value & TC32_CPSR_MODE_MASK;

    if (old_mode != new_mode) {
        env->banked_sp[old_mode] = env->regs[13];
        env->banked_lr[old_mode] = env->regs[14];
        env->regs[13] = env->banked_sp[new_mode];
        env->regs[14] = env->banked_lr[new_mode];
    }
    env->cpsr = value;
}
