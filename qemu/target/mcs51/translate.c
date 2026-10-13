#include "qemu/osdep.h"
#include "cpu.h"
#include "uc_priv.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "exec/translator.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "tcg/tcg-op.h"

#define DISAS_UC_EXIT DISAS_TARGET_6

typedef struct DisasContext {
    DisasContextBase base;
    MCS51CPUState *env;
    struct uc_struct *uc;
} DisasContext;

static void gen_save_pc(DisasContext *ctx, target_ulong pc)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 value = tcg_const_i32(tcg_ctx, pc & 0xffffu);
    tcg_gen_st_i32(tcg_ctx, value, tcg_ctx->cpu_env,
                   offsetof(MCS51CPUState, pc));
    tcg_temp_free_i32(tcg_ctx, value);
}

static uint32_t mcs51_insn_size(uint8_t opcode)
{
    if ((opcode & 0x1fu) == 0x01u || (opcode & 0x1fu) == 0x11u)
        return 2;

    switch (opcode) {
    case 0x02: case 0x10: case 0x12: case 0x20: case 0x30:
    case 0x43: case 0x53: case 0x63: case 0x75: case 0x85:
    case 0x90: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc:
    case 0xbd: case 0xbe: case 0xbf: case 0xd5:
        return 3;
    case 0x05: case 0x15: case 0x24: case 0x25: case 0x34: case 0x35:
    case 0x94: case 0x95:
    case 0x40: case 0x42: case 0x44: case 0x45: case 0x50: case 0x52:
    case 0x54: case 0x55: case 0x60: case 0x62: case 0x64: case 0x65:
    case 0x70: case 0x72: case 0x74: case 0x76: case 0x77: case 0x80:
    case 0x82: case 0x86: case 0x87: case 0xa0: case 0xa2: case 0xa6:
    case 0xa7: case 0xb0: case 0xb2: case 0xc0: case 0xc2: case 0xc5:
    case 0xc6: case 0xc7: case 0xd0: case 0xd2: case 0xd8: case 0xd9:
    case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
    case 0xe5: case 0xf5:
        return 2;
    default:
        if ((opcode >= 0x78 && opcode <= 0x7f) ||
            (opcode >= 0x88 && opcode <= 0x8f) ||
            (opcode >= 0xa8 && opcode <= 0xaf) ||
            (opcode >= 0xe8 && opcode <= 0xef) ||
            (opcode >= 0xf8 && opcode <= 0xff))
            return 2;
        return 1;
    }
}

static void mcs51_tr_init_disas_context(DisasContextBase *dcbase,
                                        CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    ctx->uc = cpu->uc;
    ctx->env = cpu->env_ptr;
}

static void mcs51_tr_tb_start(DisasContextBase *dcbase, CPUState *cpu)
{
    (void)dcbase;
    (void)cpu;
}

static void mcs51_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->uc->tcg_ctx, ctx->base.pc_next);
    (void)cpu;
}

static void mcs51_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    target_ulong pc = ctx->base.pc_next & 0xffffu;
    uint8_t opcode;
    uint32_t size;

    if (uc_addr_is_exit(ctx->uc, pc)) {
        ctx->base.is_jmp = DISAS_UC_EXIT;
        return;
    }

    opcode = cpu_ldub_code(ctx->env, pc);
    size = mcs51_insn_size(opcode);
    if (HOOK_EXISTS_BOUNDED(ctx->uc, UC_HOOK_CODE, pc)) {
        gen_save_pc(ctx, pc);
        gen_uc_tracecode(tcg_ctx, size, UC_HOOK_CODE_IDX, ctx->uc, pc);
        check_exit_request(tcg_ctx);
    }

    ctx->base.pc_next = (pc + size) & 0xffffu;
    if (ctx->env->fast.idata && !HOOK_EXISTS(ctx->uc, UC_HOOK_CODE)) {
        /* No hook wants to see each instruction: the helper runs a stretch of
           them and stops the emulation when the host needs control. */
        gen_helper_uc_mcs51_run(tcg_ctx, tcg_ctx->cpu_env);
    } else {
        gen_helper_uc_mcs51_step(tcg_ctx, tcg_ctx->cpu_env);
    }
    /* End every TB after one instruction so PC, hooks and count limits stay
       synchronized with the dynamically executed MCS-51 instruction. */
    tcg_gen_exit_tb(tcg_ctx, NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    (void)cpu;
}

static void mcs51_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    (void)cpu;

    if (ctx->base.is_jmp == DISAS_UC_EXIT) {
        gen_save_pc(ctx, ctx->base.pc_next);
        gen_helper_uc_mcs51_exit(ctx->uc->tcg_ctx,
                                 ctx->uc->tcg_ctx->cpu_env);
    } else if (ctx->base.is_jmp != DISAS_NORETURN) {
        gen_save_pc(ctx, ctx->base.pc_next);
        tcg_gen_exit_tb(ctx->uc->tcg_ctx, NULL, 0);
    }
}

static void mcs51_pc_sync(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    gen_save_pc(ctx, ctx->base.pc_next);
    (void)cpu;
}

static const TranslatorOps mcs51_tr_ops = {
    .init_disas_context = mcs51_tr_init_disas_context,
    .tb_start = mcs51_tr_tb_start,
    .insn_start = mcs51_tr_insn_start,
    .translate_insn = mcs51_tr_translate_insn,
    .tb_stop = mcs51_tr_tb_stop,
    .pc_sync = mcs51_pc_sync,
};

void gen_intermediate_code(CPUState *cpu, TranslationBlock *tb, int max_insns)
{
    DisasContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    translator_loop(&mcs51_tr_ops, &ctx.base, cpu, tb, max_insns);
}

void restore_state_to_opc(MCS51CPUState *env, TranslationBlock *tb,
                          target_ulong *data)
{
    (void)tb;
    env->pc = data[0] & 0xffffu;
}

void mcs51_tcg_init(struct uc_struct *uc)
{
    (void)uc;
}
