#include "qemu/osdep.h"
#include "cpu.h"
#include "uc_priv.h"
#include "exec/exec-all.h"

static void mcs51_cpu_set_pc(CPUState *cpu, vaddr value)
{
    MCS51_CPU(cpu)->env.pc = value & 0xffffu;
}

static void mcs51_cpu_synchronize_from_tb(CPUState *cpu, TranslationBlock *tb)
{
    MCS51_CPU(cpu)->env.pc = tb->pc & 0xffffu;
}

static void mcs51_cpu_reset(CPUState *cpu)
{
    MCS51CPU *mcs51 = MCS51_CPU(cpu);
    MCS51CPUClass *mcc = MCS51_CPU_GET_CLASS(mcs51);

    mcc->parent_reset(cpu);
    memset(&mcs51->env, 0, offsetof(MCS51CPUState, end_reset_fields));
}

static bool mcs51_cpu_has_work(CPUState *cpu)
{
    (void)cpu;
    return true;
}

static void mcs51_cpu_initfn(struct uc_struct *uc, CPUState *cpu)
{
    MCS51CPU *mcs51 = MCS51_CPU(cpu);
    mcs51->env.uc = uc;
    /* Classic 8051: P2 supplies the MOVX @Ri page (see unicorn/mcs51.h). */
    mcs51->env.xdata_page_sfr = 0xa0;
    mcs51->env.timer0_interrupt = 1;
    cpu_set_cpustate_pointers(mcs51);
}

static void mcs51_cpu_class_init(CPUClass *cc)
{
    MCS51CPUClass *mcc = MCS51_CPU_CLASS(cc);

    mcc->parent_reset = cc->reset;
    cc->reset = mcs51_cpu_reset;
    cc->has_work = mcs51_cpu_has_work;
    cc->set_pc = mcs51_cpu_set_pc;
    cc->synchronize_from_tb = mcs51_cpu_synchronize_from_tb;
    cc->get_phys_page_debug = mcs51_cpu_get_phys_page_debug;
    cc->tlb_fill_cpu = mcs51_cpu_tlb_fill;
    cc->tcg_initialize = mcs51_tcg_init;
}

MCS51CPU *cpu_mcs51_init(struct uc_struct *uc)
{
    MCS51CPU *mcs51 = qemu_memalign(8, sizeof(*mcs51));
    CPUState *cpu;
    CPUClass *cc;

    if (mcs51 == NULL) {
        return NULL;
    }
    memset(mcs51, 0, sizeof(*mcs51));
    cpu = (CPUState *)mcs51;
    cc = (CPUClass *)&mcs51->cc;
    cpu->cc = cc;
    cpu->uc = uc;
    uc->cpu = cpu;

    cpu_class_init(uc, cc);
    mcs51_cpu_class_init(cc);
    cpu_common_initfn(uc, cpu);
    mcs51_cpu_initfn(uc, cpu);
    cpu_exec_realizefn(cpu);
    cpu_reset(cpu);
    cpu_address_space_init(cpu, 0, cpu->memory);
    qemu_init_vcpu(cpu);
    return mcs51;
}
