#include "qemu/osdep.h"
#include "unicorn/unicorn.h"
#include "unicorn/mcs51.h"
#include "sysemu/cpus.h"
#include "sysemu/tcg.h"
#include "exec/cpu_ldst.h"
#include "cpu.h"
#include "uc_priv.h"
#include "unicorn_common.h"
#include "unicorn.h"

static void mcs51_set_pc(struct uc_struct *uc, uint64_t address)
{
    ((MCS51CPUState *)uc->cpu->env_ptr)->pc = address & 0xffffu;
}

static uint64_t mcs51_get_pc(struct uc_struct *uc)
{
    return ((MCS51CPUState *)uc->cpu->env_ptr)->pc & 0xffffu;
}

static void mcs51_reg_reset(struct uc_struct *uc)
{
    MCS51CPUState *env = uc->cpu->env_ptr;
    env->pc = 0;
    env->cycles = 0;
    env->instructions = 0;
    env->interrupt_vector = 0;
    env->interrupt_level = 0;
    env->interrupt_levels = 0;
    env->interrupt_inhibit = 0;
    env->event_cycles = UINT64_MAX;
    env->instruction_limit = UINT64_MAX;
    env->break_pc = 0x10000u;
    env->rmw_access = 0;
}

static uint8_t sfr_read(MCS51CPUState *env, uint8_t address)
{
    return cpu_ldub_data(env, UC_MCS51_SFR_BASE + address);
}

static void sfr_write(MCS51CPUState *env, uint8_t address, uint8_t value)
{
    cpu_stb_data(env, UC_MCS51_SFR_BASE + address, value);
}

static uint8_t current_register(MCS51CPUState *env, unsigned index)
{
    uint8_t bank = (sfr_read(env, 0xd0) >> 3) & 3u;
    return cpu_ldub_data(env, UC_MCS51_IDATA_BASE + bank * 8u + index);
}

static void current_register_write(MCS51CPUState *env, unsigned index,
                                   uint8_t value)
{
    uint8_t bank = (sfr_read(env, 0xd0) >> 3) & 3u;
    cpu_stb_data(env, UC_MCS51_IDATA_BASE + bank * 8u + index, value);
}

DEFAULT_VISIBILITY
uc_err reg_read(void *opaque, int mode, unsigned int regid, void *value,
                size_t *size)
{
    MCS51CPUState *env = opaque;
    uc_err ret = UC_ERR_ARG;
    uint16_t dptr;
    (void)mode;

    if (regid >= UC_MCS51_REG_R0 && regid <= UC_MCS51_REG_R7) {
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = current_register(env, regid - UC_MCS51_REG_R0);
        return UC_ERR_OK;
    }

    switch (regid) {
    case UC_MCS51_REG_A:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = sfr_read(env, 0xe0);
        return UC_ERR_OK;
    case UC_MCS51_REG_B:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = sfr_read(env, 0xf0);
        return UC_ERR_OK;
    case UC_MCS51_REG_PSW:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = sfr_read(env, 0xd0);
        return UC_ERR_OK;
    case UC_MCS51_REG_SP:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = sfr_read(env, 0x81);
        return UC_ERR_OK;
    case UC_MCS51_REG_DPL:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = sfr_read(env, 0x82);
        return UC_ERR_OK;
    case UC_MCS51_REG_DPH:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = sfr_read(env, 0x83);
        return UC_ERR_OK;
    case UC_MCS51_REG_DPTR:
        CHECK_REG_TYPE(uint16_t);
        dptr = (uint16_t)(sfr_read(env, 0x82) |
                          ((uint16_t)sfr_read(env, 0x83) << 8));
        *(uint16_t *)value = dptr;
        return UC_ERR_OK;
    case UC_MCS51_REG_PC:
        CHECK_REG_TYPE(uint16_t);
        *(uint16_t *)value = (uint16_t)env->pc;
        return UC_ERR_OK;
    case UC_MCS51_REG_CYCLES:
        CHECK_REG_TYPE(uint64_t);
        *(uint64_t *)value = env->cycles;
        return UC_ERR_OK;
    case UC_MCS51_REG_IRQ_VECTOR:
        CHECK_REG_TYPE(uint16_t);
        *(uint16_t *)value = env->interrupt_vector;
        return UC_ERR_OK;
    case UC_MCS51_REG_INSTRUCTIONS:
        CHECK_REG_TYPE(uint64_t);
        *(uint64_t *)value = env->instructions;
        return UC_ERR_OK;
    case UC_MCS51_REG_EVENT_CYCLES:
        CHECK_REG_TYPE(uint64_t);
        *(uint64_t *)value = env->event_cycles;
        return UC_ERR_OK;
    case UC_MCS51_REG_INSTRUCTION_LIMIT:
        CHECK_REG_TYPE(uint64_t);
        *(uint64_t *)value = env->instruction_limit;
        return UC_ERR_OK;
    case UC_MCS51_REG_BREAK_PC:
        CHECK_REG_TYPE(uint32_t);
        *(uint32_t *)value = env->break_pc;
        return UC_ERR_OK;
    case UC_MCS51_REG_XDATA_PAGE_SFR:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->xdata_page_sfr;
        return UC_ERR_OK;
    case UC_MCS51_REG_INTERRUPT_IN_SERVICE:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->interrupt_levels != 0;
        return UC_ERR_OK;
    case UC_MCS51_REG_TIMER0_INTERRUPT:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->timer0_interrupt;
        return UC_ERR_OK;
    case UC_MCS51_REG_IRQ_LEVEL:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->interrupt_level;
        return UC_ERR_OK;
    case UC_MCS51_REG_INTERRUPT_LEVELS:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->interrupt_levels;
        return UC_ERR_OK;
    case UC_MCS51_REG_POST_RETI_RULE:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->post_reti_rule;
        return UC_ERR_OK;
    case UC_MCS51_REG_INTERRUPT_INHIBIT:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->interrupt_inhibit;
        return UC_ERR_OK;
    case UC_MCS51_REG_DPS_SFR:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->dps_sfr;
        return UC_ERR_OK;
    case UC_MCS51_REG_DPL1_SFR:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->dpl1_sfr;
        return UC_ERR_OK;
    case UC_MCS51_REG_DPH1_SFR:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->dph1_sfr;
        return UC_ERR_OK;
    case UC_MCS51_REG_RMW_ACCESS:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->rmw_access;
        return UC_ERR_OK;
    case UC_MCS51_REG_CYCLE_MODEL:
        CHECK_REG_TYPE(uint8_t);
        *(uint8_t *)value = env->cycle_model;
        return UC_ERR_OK;
    default:
        CHECK_RET_DEPRECATE(ret, regid);
        return ret;
    }
}

DEFAULT_VISIBILITY
uc_err reg_write(void *opaque, int mode, unsigned int regid,
                 const void *value, size_t *size, int *setpc)
{
    MCS51CPUState *env = opaque;
    uc_err ret = UC_ERR_ARG;
    uint16_t dptr;
    (void)mode;

    if (regid >= UC_MCS51_REG_R0 && regid <= UC_MCS51_REG_R7) {
        CHECK_REG_TYPE(uint8_t);
        current_register_write(env, regid - UC_MCS51_REG_R0,
                               *(const uint8_t *)value);
        return UC_ERR_OK;
    }

    switch (regid) {
    case UC_MCS51_REG_A:
        CHECK_REG_TYPE(uint8_t);
        sfr_write(env, 0xe0, *(const uint8_t *)value);
        return UC_ERR_OK;
    case UC_MCS51_REG_B:
        CHECK_REG_TYPE(uint8_t);
        sfr_write(env, 0xf0, *(const uint8_t *)value);
        return UC_ERR_OK;
    case UC_MCS51_REG_PSW:
        CHECK_REG_TYPE(uint8_t);
        sfr_write(env, 0xd0, *(const uint8_t *)value);
        return UC_ERR_OK;
    case UC_MCS51_REG_SP:
        CHECK_REG_TYPE(uint8_t);
        sfr_write(env, 0x81, *(const uint8_t *)value);
        return UC_ERR_OK;
    case UC_MCS51_REG_DPL:
        CHECK_REG_TYPE(uint8_t);
        sfr_write(env, 0x82, *(const uint8_t *)value);
        return UC_ERR_OK;
    case UC_MCS51_REG_DPH:
        CHECK_REG_TYPE(uint8_t);
        sfr_write(env, 0x83, *(const uint8_t *)value);
        return UC_ERR_OK;
    case UC_MCS51_REG_DPTR:
        CHECK_REG_TYPE(uint16_t);
        dptr = *(const uint16_t *)value;
        sfr_write(env, 0x82, (uint8_t)dptr);
        sfr_write(env, 0x83, (uint8_t)(dptr >> 8));
        return UC_ERR_OK;
    case UC_MCS51_REG_PC:
        CHECK_REG_TYPE(uint16_t);
        env->pc = *(const uint16_t *)value;
        *setpc = 1;
        return UC_ERR_OK;
    case UC_MCS51_REG_CYCLES:
        CHECK_REG_TYPE(uint64_t);
        env->cycles = *(const uint64_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_IRQ_VECTOR:
        CHECK_REG_TYPE(uint16_t);
        env->interrupt_vector = *(const uint16_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_INSTRUCTIONS:
        CHECK_REG_TYPE(uint64_t);
        env->instructions = *(const uint64_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_FAST_MEMORY:
        CHECK_REG_TYPE(const uc_mcs51_fast_memory *);
        env->fast = **(const uc_mcs51_fast_memory *const *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_EVENT_CYCLES:
        CHECK_REG_TYPE(uint64_t);
        env->event_cycles = *(const uint64_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_INSTRUCTION_LIMIT:
        CHECK_REG_TYPE(uint64_t);
        env->instruction_limit = *(const uint64_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_BREAK_PC:
        CHECK_REG_TYPE(uint32_t);
        env->break_pc = *(const uint32_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_XDATA_PAGE_SFR:
        CHECK_REG_TYPE(uint8_t);
        if (*(const uint8_t *)value < 0x80u) {
            return UC_ERR_ARG;
        }
        env->xdata_page_sfr = *(const uint8_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_INTERRUPT_IN_SERVICE:
        CHECK_REG_TYPE(uint8_t);
        if (*(const uint8_t *)value == 0) {
            env->interrupt_levels = 0;
        } else if (env->interrupt_levels == 0) {
            env->interrupt_levels = 1;
        }
        return UC_ERR_OK;
    case UC_MCS51_REG_TIMER0_INTERRUPT:
        CHECK_REG_TYPE(uint8_t);
        env->timer0_interrupt = *(const uint8_t *)value != 0;
        return UC_ERR_OK;
    case UC_MCS51_REG_IRQ_LEVEL:
        CHECK_REG_TYPE(uint8_t);
        if (*(const uint8_t *)value > 7u) {
            return UC_ERR_ARG;
        }
        env->interrupt_level = *(const uint8_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_INTERRUPT_LEVELS:
        CHECK_REG_TYPE(uint8_t);
        env->interrupt_levels = *(const uint8_t *)value;
        return UC_ERR_OK;
    case UC_MCS51_REG_POST_RETI_RULE:
        CHECK_REG_TYPE(uint8_t);
        env->post_reti_rule = *(const uint8_t *)value != 0;
        return UC_ERR_OK;
    case UC_MCS51_REG_INTERRUPT_INHIBIT:
        CHECK_REG_TYPE(uint8_t);
        env->interrupt_inhibit = *(const uint8_t *)value != 0;
        return UC_ERR_OK;
    case UC_MCS51_REG_DPS_SFR:
    case UC_MCS51_REG_DPL1_SFR:
    case UC_MCS51_REG_DPH1_SFR:
        CHECK_REG_TYPE(uint8_t);
        if (*(const uint8_t *)value != 0 && *(const uint8_t *)value < 0x80u) {
            return UC_ERR_ARG;
        }
        if (regid == UC_MCS51_REG_DPS_SFR) {
            env->dps_sfr = *(const uint8_t *)value;
        } else if (regid == UC_MCS51_REG_DPL1_SFR) {
            env->dpl1_sfr = *(const uint8_t *)value;
        } else {
            env->dph1_sfr = *(const uint8_t *)value;
        }
        return UC_ERR_OK;
    case UC_MCS51_REG_CYCLE_MODEL:
        CHECK_REG_TYPE(uint8_t);
        if (*(const uint8_t *)value > UC_MCS51_CYCLE_MODEL_CC253X) {
            return UC_ERR_ARG;
        }
        env->cycle_model = *(const uint8_t *)value;
        return UC_ERR_OK;
    default:
        CHECK_RET_DEPRECATE(ret, regid);
        return ret;
    }
}

static int mcs51_cpus_init(struct uc_struct *uc, const char *cpu_model)
{
    (void)cpu_model;
    return cpu_mcs51_init(uc) == NULL ? -1 : 0;
}

static void mcs51_release(void *opaque)
{
    TCGContext *tcg_ctx = opaque;
    MCS51CPU *cpu = (MCS51CPU *)tcg_ctx->uc->cpu;
    CPUTLBDesc *desc = cpu->neg.tlb.d;
    CPUTLBDescFast *fast = cpu->neg.tlb.f;

    release_common(opaque);
    for (int i = 0; i < NB_MMU_MODES; i++) {
        g_free(desc[i].iotlb);
        g_free(fast[i].table);
    }
}

DEFAULT_VISIBILITY
void uc_init(struct uc_struct *uc)
{
    uc->reg_read = reg_read;
    uc->reg_write = reg_write;
    uc->reg_reset = mcs51_reg_reset;
    uc->set_pc = mcs51_set_pc;
    uc->get_pc = mcs51_get_pc;
    uc->cpus_init = mcs51_cpus_init;
    uc->release = mcs51_release;
    uc->cpu_context_size = offsetof(MCS51CPUState, end_reset_fields);
    uc_common_init(uc);
}
