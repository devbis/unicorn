#include "qemu/osdep.h"
#include "unicorn/unicorn.h"
#include "sysemu/cpus.h"
#include "sysemu/tcg.h"
#include "cpu.h"
#include "uc_priv.h"
#include "unicorn_common.h"
#include "unicorn.h"

static void tc32_set_pc(struct uc_struct *uc, uint64_t address)
{
    ((TC32CPUState *)uc->cpu->env_ptr)->regs[15] = address & ~1ull;
}

static uint64_t tc32_get_pc(struct uc_struct *uc)
{
    return ((TC32CPUState *)uc->cpu->env_ptr)->regs[15];
}

static void tc32_reg_reset(struct uc_struct *uc)
{
    TC32CPUState *env = uc->cpu->env_ptr;
    memset(env->regs, 0, sizeof(env->regs));
    memset(env->banked_sp, 0, sizeof(env->banked_sp));
    memset(env->banked_lr, 0, sizeof(env->banked_lr));
    memset(env->spsr, 0, sizeof(env->spsr));
    env->cpsr = TC32_CPSR_MODE_SVC;
    env->ssr = 0;
}

DEFAULT_VISIBILITY
uc_err uc_tc32_set_fetch_alias(uc_engine *uc, uint64_t alias_base,
                               uint64_t size, uint64_t source_base)
{
    TC32CPUState *env;

    if (uc == NULL || uc->cpu == NULL) {
        return UC_ERR_HANDLE;
    }
    if (uc->arch != UC_ARCH_TC32) {
        return UC_ERR_ARCH;
    }
    if (alias_base > UINT32_MAX || source_base > UINT32_MAX ||
        size > (UINT64_C(1) << 32) ||
        alias_base + size > (UINT64_C(1) << 32) ||
        source_base + size > (UINT64_C(1) << 32)) {
        return UC_ERR_ARG;
    }

    env = uc->cpu->env_ptr;
    env->fetch_alias_base = alias_base;
    env->fetch_alias_size = size;
    env->fetch_alias_source = source_base;
    return UC_ERR_OK;
}

DEFAULT_VISIBILITY
uc_err reg_read(void *opaque, int mode, unsigned int regid, void *value,
                size_t *size)
{
    TC32CPUState *env = opaque;
    uc_err ret = UC_ERR_ARG;
    (void)mode;

    if (regid >= UC_TC32_REG_R0 && regid <= UC_TC32_REG_R15) {
        CHECK_REG_TYPE(uint32_t);
        *(uint32_t *)value = env->regs[regid - UC_TC32_REG_R0];
        ret = UC_ERR_OK;
    } else {
        switch (regid) {
        case UC_TC32_REG_CPSR:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = env->cpsr;
            ret = UC_ERR_OK;
            break;
        case UC_TC32_REG_SPSR:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = env->spsr[env->cpsr & TC32_CPSR_MODE_MASK];
            ret = UC_ERR_OK;
            break;
        case UC_TC32_REG_SSR:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = env->ssr;
            ret = UC_ERR_OK;
            break;
        default:
            break;
        }
    }

    CHECK_RET_DEPRECATE(ret, regid);
    return ret;
}

DEFAULT_VISIBILITY
uc_err reg_write(void *opaque, int mode, unsigned int regid,
                 const void *value, size_t *size, int *setpc)
{
    TC32CPUState *env = opaque;
    uc_err ret = UC_ERR_ARG;
    (void)mode;

    if (regid >= UC_TC32_REG_R0 && regid <= UC_TC32_REG_R15) {
        CHECK_REG_TYPE(uint32_t);
        unsigned index = regid - UC_TC32_REG_R0;
        env->regs[index] = index == 15 ? (*(const uint32_t *)value & ~1u)
                                       : *(const uint32_t *)value;
        if (index == 15) {
            *setpc = 1;
        }
        ret = UC_ERR_OK;
    } else {
        switch (regid) {
        case UC_TC32_REG_CPSR:
            CHECK_REG_TYPE(uint32_t);
            tc32_write_cpsr(env, *(const uint32_t *)value);
            ret = UC_ERR_OK;
            break;
        case UC_TC32_REG_SPSR:
            CHECK_REG_TYPE(uint32_t);
            env->spsr[env->cpsr & TC32_CPSR_MODE_MASK] =
                *(const uint32_t *)value;
            ret = UC_ERR_OK;
            break;
        case UC_TC32_REG_SSR:
            CHECK_REG_TYPE(uint32_t);
            env->ssr = *(const uint32_t *)value;
            ret = UC_ERR_OK;
            break;
        default:
            break;
        }
    }

    CHECK_RET_DEPRECATE(ret, regid);
    return ret;
}

static int tc32_cpus_init(struct uc_struct *uc, const char *cpu_model)
{
    (void)cpu_model;
    return cpu_tc32_init(uc) == NULL ? -1 : 0;
}

static void tc32_release(void *opaque)
{
    TCGContext *tcg_ctx = opaque;
    TC32CPU *cpu = (TC32CPU *)tcg_ctx->uc->cpu;
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
    uc->reg_reset = tc32_reg_reset;
    uc->set_pc = tc32_set_pc;
    uc->get_pc = tc32_get_pc;
    uc->cpus_init = tc32_cpus_init;
    uc->release = tc32_release;
    uc->cpu_context_size = offsetof(TC32CPUState, end_reset_fields);
    uc_common_init(uc);
}
