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
    TC32CPUState *env;
    struct uc_struct *uc;
    target_ulong insn_pc;
    target_ulong next_pc;
    uint16_t insn;
    int mem_idx;
} DisasContext;

static void gen_save_pc(DisasContext *ctx, target_ulong pc)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[15], pc & ~1u);
}

static void gen_exit_to(DisasContext *ctx, target_ulong pc)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    gen_save_pc(ctx, pc);
    tcg_gen_exit_tb(tcg_ctx, NULL, 0);
}

static void gen_read_reg(DisasContext *ctx, TCGv_i32 dst, unsigned reg,
                         target_ulong pc)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    if ((reg & 15u) == 15u) {
        tcg_gen_movi_i32(tcg_ctx, dst, (pc + 4) & ~1u);
    } else {
        tcg_gen_mov_i32(tcg_ctx, dst, tcg_ctx->cpu_R[reg & 15u]);
    }
}

static void gen_set_nz(DisasContext *ctx, TCGv_i32 value)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    tcg_gen_andi_i32(tcg_ctx, cpsr, cpsr,
                     ~(UC_TC32_FLAG_N | UC_TC32_FLAG_Z));
    tcg_gen_andi_i32(tcg_ctx, tmp, value, UC_TC32_FLAG_N);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_EQ, tmp, value, 0);
    tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 30);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);

    tcg_temp_free_i32(tcg_ctx, tmp);
}

static void gen_add_flags(DisasContext *ctx, TCGv_i32 lhs, TCGv_i32 rhs,
                          TCGv_i32 result)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 overflow = tcg_temp_new_i32(tcg_ctx);

    gen_set_nz(ctx, result);
    tcg_gen_andi_i32(tcg_ctx, cpsr, cpsr, ~(UC_TC32_FLAG_C | UC_TC32_FLAG_V));

    tcg_gen_setcond_i32(tcg_ctx, TCG_COND_LTU, tmp, result, lhs);
    tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 29);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);

    tcg_gen_xor_i32(tcg_ctx, tmp, lhs, rhs);
    tcg_gen_not_i32(tcg_ctx, tmp, tmp);
    tcg_gen_xor_i32(tcg_ctx, overflow, lhs, result);
    tcg_gen_and_i32(tcg_ctx, tmp, tmp, overflow);
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, UC_TC32_FLAG_N);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, overflow, tmp, 0);
    tcg_gen_shli_i32(tcg_ctx, overflow, overflow, 28);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, overflow);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, overflow);
}

static void gen_sub_flags(DisasContext *ctx, TCGv_i32 lhs, TCGv_i32 rhs,
                          TCGv_i32 result)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 overflow = tcg_temp_new_i32(tcg_ctx);

    gen_set_nz(ctx, result);
    tcg_gen_andi_i32(tcg_ctx, cpsr, cpsr, ~(UC_TC32_FLAG_C | UC_TC32_FLAG_V));

    tcg_gen_setcond_i32(tcg_ctx, TCG_COND_GEU, tmp, lhs, rhs);
    tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 29);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);

    tcg_gen_xor_i32(tcg_ctx, tmp, lhs, rhs);
    tcg_gen_xor_i32(tcg_ctx, overflow, lhs, result);
    tcg_gen_and_i32(tcg_ctx, tmp, tmp, overflow);
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, UC_TC32_FLAG_N);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, overflow, tmp, 0);
    tcg_gen_shli_i32(tcg_ctx, overflow, overflow, 28);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, overflow);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, overflow);
}

static void gen_addc_flags(DisasContext *ctx, TCGv_i32 lhs, TCGv_i32 rhs,
                           TCGv_i32 carry_in, TCGv_i32 result)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 overflow = tcg_temp_new_i32(tcg_ctx);
    TCGv_i64 lhs64 = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rhs64 = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 cin64 = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 wide = tcg_temp_new_i64(tcg_ctx);

    gen_set_nz(ctx, result);
    tcg_gen_andi_i32(tcg_ctx, cpsr, cpsr,
                     ~(UC_TC32_FLAG_C | UC_TC32_FLAG_V));
    tcg_gen_extu_i32_i64(tcg_ctx, lhs64, lhs);
    tcg_gen_extu_i32_i64(tcg_ctx, rhs64, rhs);
    tcg_gen_extu_i32_i64(tcg_ctx, cin64, carry_in);
    tcg_gen_add_i64(tcg_ctx, wide, lhs64, rhs64);
    tcg_gen_add_i64(tcg_ctx, wide, wide, cin64);
    tcg_gen_setcondi_i64(tcg_ctx, TCG_COND_GTU, cin64, wide, UINT32_MAX);
    tcg_gen_extrl_i64_i32(tcg_ctx, tmp, cin64);
    tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 29);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);

    tcg_gen_xor_i32(tcg_ctx, tmp, lhs, rhs);
    tcg_gen_not_i32(tcg_ctx, tmp, tmp);
    tcg_gen_xor_i32(tcg_ctx, overflow, lhs, result);
    tcg_gen_and_i32(tcg_ctx, tmp, tmp, overflow);
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, UC_TC32_FLAG_N);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, overflow, tmp, 0);
    tcg_gen_shli_i32(tcg_ctx, overflow, overflow, 28);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, overflow);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, overflow);
    tcg_temp_free_i64(tcg_ctx, lhs64);
    tcg_temp_free_i64(tcg_ctx, rhs64);
    tcg_temp_free_i64(tcg_ctx, cin64);
    tcg_temp_free_i64(tcg_ctx, wide);
}

static void gen_subc_flags(DisasContext *ctx, TCGv_i32 lhs, TCGv_i32 rhs,
                           TCGv_i32 borrow_in, TCGv_i32 result)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 overflow = tcg_temp_new_i32(tcg_ctx);
    TCGv_i64 lhs64 = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rhs64 = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 bin64 = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rhs_bin64 = tcg_temp_new_i64(tcg_ctx);

    gen_set_nz(ctx, result);
    tcg_gen_andi_i32(tcg_ctx, cpsr, cpsr,
                     ~(UC_TC32_FLAG_C | UC_TC32_FLAG_V));
    tcg_gen_extu_i32_i64(tcg_ctx, lhs64, lhs);
    tcg_gen_extu_i32_i64(tcg_ctx, rhs64, rhs);
    tcg_gen_extu_i32_i64(tcg_ctx, bin64, borrow_in);
    tcg_gen_add_i64(tcg_ctx, rhs_bin64, rhs64, bin64);
    tcg_gen_setcond_i64(tcg_ctx, TCG_COND_GEU, bin64, lhs64, rhs_bin64);
    tcg_gen_extrl_i64_i32(tcg_ctx, tmp, bin64);
    tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 29);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);

    tcg_gen_xor_i32(tcg_ctx, tmp, lhs, rhs);
    tcg_gen_xor_i32(tcg_ctx, overflow, lhs, result);
    tcg_gen_and_i32(tcg_ctx, tmp, tmp, overflow);
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, UC_TC32_FLAG_N);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, overflow, tmp, 0);
    tcg_gen_shli_i32(tcg_ctx, overflow, overflow, 28);
    tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, overflow);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, overflow);
    tcg_temp_free_i64(tcg_ctx, lhs64);
    tcg_temp_free_i64(tcg_ctx, rhs64);
    tcg_temp_free_i64(tcg_ctx, bin64);
    tcg_temp_free_i64(tcg_ctx, rhs_bin64);
}

static void gen_shift_flags(DisasContext *ctx, TCGv_i32 result,
                            TCGv_i32 carry, bool update_carry)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    gen_set_nz(ctx, result);
    if (update_carry) {
        tcg_gen_andi_i32(tcg_ctx, cpsr, cpsr, ~UC_TC32_FLAG_C);
        tcg_gen_andi_i32(tcg_ctx, tmp, carry, 1);
        tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 29);
        tcg_gen_or_i32(tcg_ctx, cpsr, cpsr, tmp);
    }
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static void gen_shift_register(DisasContext *ctx, unsigned op, TCGv_i32 lhs,
                               TCGv_i32 rhs, TCGv_i32 result)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 amount = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 shift = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 carry = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 carry_nonzero = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 carry_raw = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 edge32 = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 zero = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 limit = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 old_carry = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 raw_result = tcg_temp_new_i32(tcg_ctx);

    tcg_gen_andi_i32(tcg_ctx, amount, rhs, 0xff);
    tcg_gen_movi_i32(tcg_ctx, zero, 0);
    tcg_gen_movi_i32(tcg_ctx, limit, 32);
    tcg_gen_movi_i32(tcg_ctx, carry_nonzero, 0);

    if (op == 0x01c0u) {
        tcg_gen_andi_i32(tcg_ctx, shift, amount, 31);
        tcg_gen_rotr_i32(tcg_ctx, raw_result, lhs, shift);
        tcg_gen_mov_i32(tcg_ctx, result, raw_result);
        tcg_gen_shri_i32(tcg_ctx, carry_raw, raw_result, 31);
        tcg_gen_movcond_i32(tcg_ctx, TCG_COND_NE, carry_nonzero, amount,
                            zero, carry_raw, zero);
    } else {
        tcg_gen_andi_i32(tcg_ctx, shift, amount, 31);
        if (op == 0x0080u) {
            tcg_gen_shl_i32(tcg_ctx, raw_result, lhs, shift);
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LTU, result, amount,
                                limit, raw_result, zero);
            tcg_gen_subfi_i32(tcg_ctx, shift, 32, shift);
            tcg_gen_andi_i32(tcg_ctx, shift, shift, 31);
            tcg_gen_shr_i32(tcg_ctx, carry_raw, lhs, shift);
            tcg_gen_andi_i32(tcg_ctx, carry_raw, carry_raw, 1);
            tcg_gen_andi_i32(tcg_ctx, edge32, lhs, 1);
        } else {
            if (op == 0x0100u) {
                tcg_gen_sar_i32(tcg_ctx, raw_result, lhs, shift);
            } else {
                tcg_gen_shr_i32(tcg_ctx, raw_result, lhs, shift);
            }
            if (op == 0x0100u) {
                tcg_gen_sari_i32(tcg_ctx, edge32, lhs, 31);
            } else {
                tcg_gen_movi_i32(tcg_ctx, edge32, 0);
            }
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LTU, result, amount,
                                limit, raw_result, edge32);
            tcg_gen_subi_i32(tcg_ctx, shift, shift, 1);
            tcg_gen_andi_i32(tcg_ctx, shift, shift, 31);
            tcg_gen_shr_i32(tcg_ctx, carry_raw, lhs, shift);
            tcg_gen_andi_i32(tcg_ctx, carry_raw, carry_raw, 1);
        }
        if (op != 0x0080u) {
            tcg_gen_shri_i32(tcg_ctx, edge32, lhs, 31);
        }
        if (op == 0x0100u) {
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_GEU, carry_nonzero,
                                amount, limit, edge32, carry_raw);
        } else {
            tcg_gen_movcond_i32(tcg_ctx, TCG_COND_EQ, carry_nonzero,
                                amount, limit, edge32, carry_raw);
        }
    }
    tcg_gen_andi_i32(tcg_ctx, old_carry, tcg_ctx->cpu_tc32_cpsr,
                     UC_TC32_FLAG_C);
    tcg_gen_shri_i32(tcg_ctx, old_carry, old_carry, 29);
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_EQ, carry, amount, zero,
                        old_carry, carry_nonzero);
    gen_shift_flags(ctx, result, carry, true);

    tcg_temp_free_i32(tcg_ctx, amount);
    tcg_temp_free_i32(tcg_ctx, shift);
    tcg_temp_free_i32(tcg_ctx, carry);
    tcg_temp_free_i32(tcg_ctx, carry_nonzero);
    tcg_temp_free_i32(tcg_ctx, carry_raw);
    tcg_temp_free_i32(tcg_ctx, edge32);
    tcg_temp_free_i32(tcg_ctx, zero);
    tcg_temp_free_i32(tcg_ctx, limit);
    tcg_temp_free_i32(tcg_ctx, old_carry);
    tcg_temp_free_i32(tcg_ctx, raw_result);
}

static TCGv_i32 gen_condition(DisasContext *ctx, unsigned cond)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGv_i32 cpsr = tcg_ctx->cpu_tc32_cpsr;
    TCGv_i32 n = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 z = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 c = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 v = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 result = tcg_temp_new_i32(tcg_ctx);

    tcg_gen_andi_i32(tcg_ctx, n, cpsr, UC_TC32_FLAG_N);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, n, n, 0);
    tcg_gen_andi_i32(tcg_ctx, z, cpsr, UC_TC32_FLAG_Z);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, z, z, 0);
    tcg_gen_andi_i32(tcg_ctx, c, cpsr, UC_TC32_FLAG_C);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, c, c, 0);
    tcg_gen_andi_i32(tcg_ctx, v, cpsr, UC_TC32_FLAG_V);
    tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_NE, v, v, 0);

    switch (cond & 15u) {
    case 0: tcg_gen_mov_i32(tcg_ctx, result, z); break;
    case 1: tcg_gen_xori_i32(tcg_ctx, result, z, 1); break;
    case 2: tcg_gen_mov_i32(tcg_ctx, result, c); break;
    case 3: tcg_gen_xori_i32(tcg_ctx, result, c, 1); break;
    case 4: tcg_gen_mov_i32(tcg_ctx, result, n); break;
    case 5: tcg_gen_xori_i32(tcg_ctx, result, n, 1); break;
    case 6: tcg_gen_mov_i32(tcg_ctx, result, v); break;
    case 7: tcg_gen_xori_i32(tcg_ctx, result, v, 1); break;
    case 8:
        tcg_gen_xori_i32(tcg_ctx, z, z, 1);
        tcg_gen_and_i32(tcg_ctx, result, c, z);
        break;
    case 9:
        /* LS = !C || Z */
        tcg_gen_xori_i32(tcg_ctx, c, c, 1);
        tcg_gen_or_i32(tcg_ctx, result, c, z);
        break;
    case 10: tcg_gen_setcond_i32(tcg_ctx, TCG_COND_EQ, result, n, v); break;
    case 11: tcg_gen_setcond_i32(tcg_ctx, TCG_COND_NE, result, n, v); break;
    case 12:
        tcg_gen_setcond_i32(tcg_ctx, TCG_COND_EQ, result, n, v);
        tcg_gen_xori_i32(tcg_ctx, z, z, 1);
        tcg_gen_and_i32(tcg_ctx, result, result, z);
        break;
    case 13:
        tcg_gen_setcond_i32(tcg_ctx, TCG_COND_NE, result, n, v);
        tcg_gen_or_i32(tcg_ctx, result, result, z);
        break;
    default:
        tcg_gen_movi_i32(tcg_ctx, result, cond == 14);
        break;
    }

    tcg_temp_free_i32(tcg_ctx, n);
    tcg_temp_free_i32(tcg_ctx, z);
    tcg_temp_free_i32(tcg_ctx, c);
    tcg_temp_free_i32(tcg_ctx, v);
    return result;
}

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    uint32_t mask = (1u << bits) - 1u;
    value &= mask;
    return (int32_t)((value ^ sign) - sign);
}

static unsigned tc32_count_bits(uint32_t value)
{
    unsigned count = 0;
    while (value) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

static target_ulong tc32_fetch_address(const TC32CPUState *env,
                                       target_ulong address)
{
    if (env->fetch_alias_size != 0 && address >= env->fetch_alias_base &&
        address - env->fetch_alias_base < env->fetch_alias_size) {
        return env->fetch_alias_source +
               (address - env->fetch_alias_base);
    }
    return address;
}

static void gen_conditional_branch(DisasContext *ctx, unsigned cond,
                                   target_ulong target,
                                   target_ulong fallthrough)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    TCGLabel *not_taken = gen_new_label(tcg_ctx);
    TCGv_i32 condition = gen_condition(ctx, cond);

    tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_EQ, condition, 0, not_taken);
    tcg_temp_free_i32(tcg_ctx, condition);
    gen_exit_to(ctx, target);
    gen_set_label(tcg_ctx, not_taken);
    gen_exit_to(ctx, fallthrough);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_load(DisasContext *ctx, unsigned rd, TCGv_i32 address,
                     MemOp memop)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    if (rd == 15) {
        TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_qemu_ld_i32(tcg_ctx, tmp, address, ctx->mem_idx, memop);
        tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[15], tmp, ~1u);
        tcg_temp_free_i32(tcg_ctx, tmp);
        tcg_gen_exit_tb(tcg_ctx, NULL, 0);
        ctx->base.is_jmp = DISAS_NORETURN;
    } else {
        tcg_gen_qemu_ld_i32(tcg_ctx, tcg_ctx->cpu_R[rd], address,
                            ctx->mem_idx, memop);
    }
}

static void gen_store(DisasContext *ctx, unsigned rs, TCGv_i32 address,
                      MemOp memop)
{
    tcg_gen_qemu_st_i32(ctx->uc->tcg_ctx,
                        ctx->uc->tcg_ctx->cpu_R[rs], address,
                        ctx->mem_idx, memop);
}

static bool tc32_insn_is_long(uint16_t insn)
{
    return (insn & 0xf800u) == 0x9000u;
}

static void tc32_translate_16(DisasContext *ctx)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    uint16_t insn = ctx->insn;
    unsigned rd, rn, rm;
    TCGv_i32 lhs, rhs, result, address;

    if ((insn & 0xf000u) == 0x8000u) {
        target_ulong target = ctx->insn_pc + 4 +
            (int64_t)sign_extend(insn & 0x07ffu, 11) * 2;
        gen_exit_to(ctx, target);
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }
    if ((insn & 0xff00u) >= 0xc000u && (insn & 0xff00u) <= 0xcd00u) {
        unsigned cond = (insn >> 8) & 15u;
        target_ulong target = ctx->insn_pc + 4 +
            (int64_t)sign_extend(insn & 0xffu, 8) * 2;
        gen_conditional_branch(ctx, cond, target, ctx->next_pc);
        return;
    }
    if ((insn & 0xff00u) == 0xcf00u) {
        gen_save_pc(ctx, ctx->insn_pc);
        gen_helper_uc_tc32_invalid(tcg_ctx, tcg_ctx->cpu_env);
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }
    if ((insn & 0xf800u) == 0xa000u) {
        rd = (insn >> 8) & 7u;
        tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[rd], insn & 0xffu);
        gen_set_nz(ctx, tcg_ctx->cpu_R[rd]);
        return;
    }
    if ((insn & 0xfe00u) == 0x6400u ||
        (insn & 0xfe00u) == 0x6c00u) {
        bool is_pop = (insn & 0xfe00u) == 0x6c00u;
        bool extra_reg = (insn & 0x0100u) != 0;
        uint32_t mask = insn & 0xffu;
        unsigned count = tc32_count_bits(mask) + extra_reg;
        unsigned offset = 0;
        TCGv_i32 stack_address = tcg_temp_new_i32(tcg_ctx);
        TCGv_i32 value = tcg_temp_new_i32(tcg_ctx);

        if (!count) {
            tcg_temp_free_i32(tcg_ctx, stack_address);
            tcg_temp_free_i32(tcg_ctx, value);
            goto invalid;
        }
        if (!is_pop) {
            tcg_gen_subi_i32(tcg_ctx, tcg_ctx->cpu_R[13],
                             tcg_ctx->cpu_R[13], count * 4);
            for (unsigned reg = 0; reg < 8; reg++) {
                if (mask & (1u << reg)) {
                    tcg_gen_addi_i32(tcg_ctx, stack_address,
                                     tcg_ctx->cpu_R[13], offset);
                    tcg_gen_qemu_st_i32(tcg_ctx, tcg_ctx->cpu_R[reg],
                                        stack_address, ctx->mem_idx, MO_LEUL);
                    offset += 4;
                }
            }
            if (extra_reg) {
                tcg_gen_addi_i32(tcg_ctx, stack_address,
                                 tcg_ctx->cpu_R[13], offset);
                tcg_gen_qemu_st_i32(tcg_ctx, tcg_ctx->cpu_R[14],
                                    stack_address, ctx->mem_idx, MO_LEUL);
            }
        } else {
            bool pc_in_list = extra_reg;
            for (unsigned reg = 0; reg < 8; reg++) {
                if (mask & (1u << reg)) {
                    tcg_gen_addi_i32(tcg_ctx, stack_address,
                                     tcg_ctx->cpu_R[13], offset);
                    tcg_gen_qemu_ld_i32(tcg_ctx, tcg_ctx->cpu_R[reg],
                                        stack_address, ctx->mem_idx, MO_LEUL);
                    offset += 4;
                }
            }
            if (pc_in_list) {
                tcg_gen_addi_i32(tcg_ctx, stack_address,
                                 tcg_ctx->cpu_R[13], offset);
                tcg_gen_qemu_ld_i32(tcg_ctx, value, stack_address,
                                    ctx->mem_idx, MO_LEUL);
                offset += 4;
            }
            tcg_gen_addi_i32(tcg_ctx, tcg_ctx->cpu_R[13],
                             tcg_ctx->cpu_R[13], offset);
            if (pc_in_list) {
                tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[15], value, ~1u);
                tcg_gen_exit_tb(tcg_ctx, NULL, 0);
                ctx->base.is_jmp = DISAS_NORETURN;
            }
        }
        tcg_temp_free_i32(tcg_ctx, stack_address);
        tcg_temp_free_i32(tcg_ctx, value);
        return;
    }
    if (insn == 0x6900u) {
        gen_helper_uc_tc32_treti(tcg_ctx, tcg_ctx->cpu_env);
        tcg_gen_exit_tb(tcg_ctx, NULL, 0);
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }
    if ((insn & 0xfff8u) == 0x6bc0u) {
        rd = insn & 7u;
        gen_helper_uc_tc32_write_cpsr(tcg_ctx, tcg_ctx->cpu_env,
                                       tcg_ctx->cpu_R[rd]);
        return;
    }
    if ((insn & 0xfff8u) == 0x6bc8u) {
        rd = insn & 7u;
        tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd],
                        tcg_ctx->cpu_tc32_cpsr);
        return;
    }
    if ((insn & 0xfff8u) == 0x6bd0u) {
        rd = insn & 7u;
        tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_tc32_ssr,
                        tcg_ctx->cpu_R[rd]);
        return;
    }
    if ((insn & 0xfff8u) == 0x6bd8u) {
        rd = insn & 7u;
        tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd],
                        tcg_ctx->cpu_tc32_ssr);
        return;
    }
    if ((insn & 0xf800u) == 0xa800u) {
        rn = (insn >> 8) & 7u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rn, ctx->insn_pc);
        tcg_gen_movi_i32(tcg_ctx, rhs, insn & 0xffu);
        tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
        gen_sub_flags(ctx, lhs, rhs, result);
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if ((insn & 0xf800u) == 0xb000u || (insn & 0xf800u) == 0xb800u) {
        bool is_add = (insn & 0x0800u) == 0;
        rd = (insn >> 8) & 7u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rd, ctx->insn_pc);
        tcg_gen_movi_i32(tcg_ctx, rhs, insn & 0xffu);
        if (is_add) {
            tcg_gen_add_i32(tcg_ctx, result, lhs, rhs);
            tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
            gen_add_flags(ctx, lhs, rhs, result);
        } else {
            tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
            tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
            gen_sub_flags(ctx, lhs, rhs, result);
        }
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if ((insn & 0xff80u) == 0x6000u || (insn & 0xff80u) == 0x6080u) {
        uint32_t bytes = (insn & 0x7fu) * 4u;
        if (insn & 0x80u) {
            tcg_gen_subi_i32(tcg_ctx, tcg_ctx->cpu_R[13],
                             tcg_ctx->cpu_R[13], bytes);
        } else {
            tcg_gen_addi_i32(tcg_ctx, tcg_ctx->cpu_R[13],
                             tcg_ctx->cpu_R[13], bytes);
        }
        return;
    }
    if ((insn & 0xf800u) == 0x7800u) {
        rd = (insn >> 8) & 7u;
        tcg_gen_addi_i32(tcg_ctx, tcg_ctx->cpu_R[rd],
                         tcg_ctx->cpu_R[13], (insn & 0xffu) << 2);
        return;
    }
    if ((insn & 0xf800u) == 0xd000u ||
        (insn & 0xf800u) == 0xd800u) {
        bool is_load = (insn & 0x0800u) != 0;
        rn = (insn >> 8) & 7u;
        uint32_t mask = insn & 0xffu;
        unsigned count = tc32_count_bits(mask);
        unsigned offset = 0;
        TCGv_i32 address = tcg_temp_new_i32(tcg_ctx);

        if (!count) {
            tcg_temp_free_i32(tcg_ctx, address);
            goto invalid;
        }
        for (unsigned reg = 0; reg < 8; reg++) {
            if (mask & (1u << reg)) {
                tcg_gen_addi_i32(tcg_ctx, address, tcg_ctx->cpu_R[rn],
                                 offset);
                if (is_load) {
                    tcg_gen_qemu_ld_i32(tcg_ctx, tcg_ctx->cpu_R[reg],
                                        address, ctx->mem_idx, MO_LEUL);
                } else {
                    tcg_gen_qemu_st_i32(tcg_ctx, tcg_ctx->cpu_R[reg],
                                        address, ctx->mem_idx, MO_LEUL);
                }
                offset += 4;
            }
        }
        tcg_gen_addi_i32(tcg_ctx, tcg_ctx->cpu_R[rn], tcg_ctx->cpu_R[rn],
                         offset);
        tcg_temp_free_i32(tcg_ctx, address);
        return;
    }
    if ((insn & 0xf800u) == 0x0800u) {
        rd = (insn >> 8) & 7u;
        address = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_movi_i32(
            tcg_ctx, address,
            tc32_fetch_address(ctx->env,
                               ((ctx->insn_pc + 4) & ~3u) +
                                   ((insn & 0xffu) << 2)));
        gen_load(ctx, rd, address, MO_LEUL);
        tcg_temp_free_i32(tcg_ctx, address);
        return;
    }
    if ((insn & 0xf000u) == 0x3000u) {
        rd = (insn >> 8) & 7u;
        bool is_load = (insn & 0x0800u) != 0;
        address = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_addi_i32(tcg_ctx, address, tcg_ctx->cpu_R[13],
                         (insn & 0xffu) << 2);
        if (is_load) {
            gen_load(ctx, rd, address, MO_LEUL);
        } else {
            gen_store(ctx, rd, address, MO_LEUL);
        }
        tcg_temp_free_i32(tcg_ctx, address);
        return;
    }
    if ((insn & 0xfe00u) == 0xe800u ||
        (insn & 0xfe00u) == 0xea00u) {
        bool is_add = (insn & 0x0600u) == 0x0000u;
        rd = insn & 7u;
        rn = (insn >> 3) & 7u;
        rm = (insn >> 6) & 7u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rn, ctx->insn_pc);
        gen_read_reg(ctx, rhs, rm, ctx->insn_pc);
        if (is_add) {
            tcg_gen_add_i32(tcg_ctx, result, lhs, rhs);
            gen_add_flags(ctx, lhs, rhs, result);
        } else {
            tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
            gen_sub_flags(ctx, lhs, rhs, result);
        }
        tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if ((insn & 0xfe00u) == 0xec00u ||
        (insn & 0xfe00u) == 0xee00u) {
        bool is_add = (insn & 0x0200u) == 0;
        unsigned imm = (insn >> 6) & 7u;
        rd = insn & 7u;
        rn = (insn >> 3) & 7u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rn, ctx->insn_pc);
        tcg_gen_movi_i32(tcg_ctx, rhs, imm);
        if (is_add) {
            tcg_gen_add_i32(tcg_ctx, result, lhs, rhs);
            gen_add_flags(ctx, lhs, rhs, result);
        } else {
            tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
            gen_sub_flags(ctx, lhs, rhs, result);
        }
        tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if ((insn & 0xf800u) == 0xf000u ||
        (insn & 0xf800u) == 0xf800u ||
        (insn & 0xf800u) == 0xe000u) {
        unsigned shift_op = (insn & 0xf800u) == 0xf000u ? 0x0080u :
                            (insn & 0xf800u) == 0xf800u ? 0x00c0u :
                                                         0x0100u;
        unsigned amount = (insn >> 6) & 31u;
        rd = insn & 7u;
        rn = (insn >> 3) & 7u;
        if (shift_op != 0x0080u && amount == 0) {
            amount = 32;
        }
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rn, ctx->insn_pc);
        tcg_gen_movi_i32(tcg_ctx, rhs, amount);
        gen_shift_register(ctx, shift_op, lhs, rhs, result);
        tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if ((insn & 0xf800u) == 0x5000u || (insn & 0xf800u) == 0x5800u ||
        (insn & 0xf800u) == 0x4000u || (insn & 0xf800u) == 0x4800u ||
        (insn & 0xf800u) == 0x2000u || (insn & 0xf800u) == 0x2800u) {
        uint16_t op = insn & 0xf800u;
        bool is_load = (op & 0x0800u) != 0;
        unsigned width = (op == 0x5000u || op == 0x5800u) ? 4u :
                         (op == 0x2000u || op == 0x2800u) ? 2u : 1u;
        rd = insn & 7u;
        rn = (insn >> 3) & 7u;
        unsigned offset = ((insn >> 6) & 31u) * width;
        address = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_addi_i32(tcg_ctx, address, tcg_ctx->cpu_R[rn], offset);
        if (is_load) {
            MemOp memop = width == 4 ? MO_LEUL : width == 2 ? MO_LEUW : MO_UB;
            gen_load(ctx, rd, address, memop);
        } else {
            MemOp memop = width == 4 ? MO_LEUL : width == 2 ? MO_LEUW : MO_8;
            gen_store(ctx, rd, address, memop);
        }
        tcg_temp_free_i32(tcg_ctx, address);
        return;
    }
    if ((insn & 0xfe00u) >= 0x1000u && (insn & 0xfe00u) <= 0x1e00u) {
        uint16_t op = insn & 0xfe00u;
        rd = insn & 7u;
        rn = (insn >> 3) & 7u;
        rm = (insn >> 6) & 7u;
        address = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_add_i32(tcg_ctx, address, tcg_ctx->cpu_R[rn],
                        tcg_ctx->cpu_R[rm]);
        switch (op) {
        case 0x1000: gen_store(ctx, rd, address, MO_LEUL); break;
        case 0x1200: gen_store(ctx, rd, address, MO_LEUW); break;
        case 0x1400: gen_store(ctx, rd, address, MO_8); break;
        case 0x1600: gen_load(ctx, rd, address, MO_SB); break;
        case 0x1800: gen_load(ctx, rd, address, MO_LEUL); break;
        case 0x1a00: gen_load(ctx, rd, address, MO_LEUW); break;
        case 0x1c00: gen_load(ctx, rd, address, MO_UB); break;
        case 0x1e00: gen_load(ctx, rd, address, MO_LESW); break;
        default:
            tcg_temp_free_i32(tcg_ctx, address);
            goto invalid;
        }
        tcg_temp_free_i32(tcg_ctx, address);
        return;
    }
    if ((insn & 0xff87u) == 0x0700u) {
        rm = (insn >> 3) & 15u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rm, ctx->insn_pc);
        tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[15], lhs, ~1u);
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_gen_exit_tb(tcg_ctx, NULL, 0);
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }
    if ((insn & 0xf700u) == 0x0600u) {
        rd = (insn & 7u) + (((insn >> 7) & 1u) * 8u);
        rm = ((insn >> 6) & 1u) * 8u + ((insn >> 3) & 7u);
        if (rd == 15) {
            lhs = tcg_temp_new_i32(tcg_ctx);
            gen_read_reg(ctx, lhs, rm, ctx->insn_pc);
            tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[15], lhs, ~1u);
            tcg_temp_free_i32(tcg_ctx, lhs);
            tcg_gen_exit_tb(tcg_ctx, NULL, 0);
            ctx->base.is_jmp = DISAS_NORETURN;
        } else {
            gen_read_reg(ctx, tcg_ctx->cpu_R[rd], rm, ctx->insn_pc);
        }
        return;
    }
    if ((insn & 0xf700u) == 0x0400u || (insn & 0xf700u) == 0x0500u) {
        bool is_add = (insn & 0x0100u) == 0;
        rd = (insn & 7u) + (((insn >> 7) & 1u) * 8u);
        rm = (insn >> 3) & 15u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rd, ctx->insn_pc);
        gen_read_reg(ctx, rhs, rm, ctx->insn_pc);
        if (is_add) {
            tcg_gen_add_i32(tcg_ctx, result, lhs, rhs);
            if (rd == 15) {
                tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[15], result, ~1u);
                tcg_gen_exit_tb(tcg_ctx, NULL, 0);
                ctx->base.is_jmp = DISAS_NORETURN;
            } else {
                tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
            }
        } else {
            tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
            gen_sub_flags(ctx, lhs, rhs, result);
        }
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if ((insn & 0xffc0u) <= 0x03c0u) {
        uint16_t op = insn & 0xffc0u;
        rd = insn & 7u;
        rm = (insn >> 3) & 7u;
        lhs = tcg_temp_new_i32(tcg_ctx);
        rhs = tcg_temp_new_i32(tcg_ctx);
        result = tcg_temp_new_i32(tcg_ctx);
        gen_read_reg(ctx, lhs, rd, ctx->insn_pc);
        gen_read_reg(ctx, rhs, rm, ctx->insn_pc);
        switch (op) {
        case 0x0000: tcg_gen_and_i32(tcg_ctx, result, lhs, rhs); gen_set_nz(ctx, result); break;
        case 0x0040: tcg_gen_xor_i32(tcg_ctx, result, lhs, rhs); gen_set_nz(ctx, result); break;
        case 0x0080: case 0x00c0: case 0x0100: case 0x01c0:
            gen_shift_register(ctx, op, lhs, rhs, result);
            break;
        case 0x0140: {
            TCGv_i32 carry_in = tcg_temp_new_i32(tcg_ctx);
            tcg_gen_andi_i32(tcg_ctx, carry_in,
                             tcg_ctx->cpu_tc32_cpsr, UC_TC32_FLAG_C);
            tcg_gen_shri_i32(tcg_ctx, carry_in, carry_in, 29);
            tcg_gen_add_i32(tcg_ctx, result, lhs, rhs);
            tcg_gen_add_i32(tcg_ctx, result, result, carry_in);
            gen_addc_flags(ctx, lhs, rhs, carry_in, result);
            tcg_temp_free_i32(tcg_ctx, carry_in);
            break;
        }
        case 0x0180: {
            TCGv_i32 borrow_in = tcg_temp_new_i32(tcg_ctx);
            tcg_gen_andi_i32(tcg_ctx, borrow_in,
                             tcg_ctx->cpu_tc32_cpsr, UC_TC32_FLAG_C);
            tcg_gen_setcondi_i32(tcg_ctx, TCG_COND_EQ, borrow_in,
                                 borrow_in, 0);
            tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
            tcg_gen_sub_i32(tcg_ctx, result, result, borrow_in);
            gen_subc_flags(ctx, lhs, rhs, borrow_in, result);
            tcg_temp_free_i32(tcg_ctx, borrow_in);
            break;
        }
        case 0x0200: tcg_gen_and_i32(tcg_ctx, result, lhs, rhs); gen_set_nz(ctx, result); break;
        case 0x0240:
            tcg_gen_neg_i32(tcg_ctx, result, rhs);
            tcg_gen_movi_i32(tcg_ctx, lhs, 0);
            gen_sub_flags(ctx, lhs, rhs, result);
            break;
        case 0x0280:
            tcg_gen_sub_i32(tcg_ctx, result, lhs, rhs);
            gen_sub_flags(ctx, lhs, rhs, result);
            break;
        case 0x02c0:
            tcg_gen_add_i32(tcg_ctx, result, lhs, rhs);
            gen_add_flags(ctx, lhs, rhs, result);
            break;
        case 0x0300: tcg_gen_or_i32(tcg_ctx, result, lhs, rhs); gen_set_nz(ctx, result); break;
        case 0x0340: tcg_gen_mul_i32(tcg_ctx, result, lhs, rhs); gen_set_nz(ctx, result); break;
        case 0x0380:
            tcg_gen_not_i32(tcg_ctx, result, rhs);
            tcg_gen_and_i32(tcg_ctx, result, result, lhs);
            gen_set_nz(ctx, result);
            break;
        case 0x03c0: tcg_gen_not_i32(tcg_ctx, result, rhs); gen_set_nz(ctx, result); break;
        default:
            tcg_temp_free_i32(tcg_ctx, lhs);
            tcg_temp_free_i32(tcg_ctx, rhs);
            tcg_temp_free_i32(tcg_ctx, result);
            goto invalid;
        }
        if (op != 0x0200u && op != 0x0280u && op != 0x02c0u) {
            tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], result);
        }
        tcg_temp_free_i32(tcg_ctx, lhs);
        tcg_temp_free_i32(tcg_ctx, rhs);
        tcg_temp_free_i32(tcg_ctx, result);
        return;
    }
    if (insn == 0x06c0u) {
        return;
    }

invalid:
    gen_save_pc(ctx, ctx->insn_pc);
    gen_helper_uc_tc32_invalid(tcg_ctx, tcg_ctx->cpu_env);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void tc32_translate_32(DisasContext *ctx, uint16_t second)
{
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    uint16_t first = ctx->insn;
    uint32_t encoded;
    target_ulong target;
    bool link;

    if ((first & 0xf000u) == 0x9000u &&
        (second & 0xa000u) == 0x2000u &&
        (((second >> 12) ^ (second >> 14)) & 1u) == 0 &&
        (((first >> 10) ^ (second >> 11)) & 1u) == 0) {
        unsigned cond = (first >> 6) & 15u;
        unsigned upper = (first & 0x3fu) |
                         (((second >> 12) & 1u) << 6) |
                         (((first >> 10) & 1u) << 7);
        encoded = (upper << 11) | (second & 0x07ffu);
        if (cond > 13u) {
            goto invalid;
        }
        target = ctx->insn_pc + 4 +
                 (int64_t)sign_extend(encoded, 19) * 2;
        gen_conditional_branch(ctx, cond, target, ctx->next_pc);
        return;
    }

    if ((second & 0xf800u) != 0x6800u && (second & 0xf800u) != 0x9800u) {
        goto invalid;
    }
    link = (second & 0xf800u) == 0x9800u;
    encoded = ((uint32_t)(first & 0x07ffu) << 11) | (second & 0x07ffu);
    target = ctx->insn_pc + 4 + (int64_t)sign_extend(encoded, 22) * 2;
    if (link) {
        tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], ctx->insn_pc + 4);
    }
    gen_exit_to(ctx, target);
    ctx->base.is_jmp = DISAS_NORETURN;
    return;

invalid:
    gen_save_pc(ctx, ctx->insn_pc);
    gen_helper_uc_tc32_invalid(tcg_ctx, tcg_ctx->cpu_env);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void tc32_tr_init_disas_context(DisasContextBase *dcbase,
                                       CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    ctx->uc = cpu->uc;
    ctx->env = cpu->env_ptr;
    ctx->mem_idx = 0;
}

static void tc32_tr_tb_start(DisasContextBase *dcbase, CPUState *cpu)
{
    (void)dcbase;
    (void)cpu;
}

static void tc32_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->uc->tcg_ctx, ctx->base.pc_next, 0);
    (void)cpu;
}

static void tc32_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    TCGContext *tcg_ctx = ctx->uc->tcg_ctx;
    uint16_t first;
    bool is_long;
    unsigned size;

    if (uc_addr_is_exit(ctx->uc, ctx->base.pc_next)) {
        ctx->base.is_jmp = DISAS_UC_EXIT;
        return;
    }

    ctx->insn_pc = ctx->base.pc_next;
    first = cpu_lduw_code(cpu->env_ptr,
                          tc32_fetch_address(ctx->env, ctx->insn_pc));
    is_long = tc32_insn_is_long(first);
    size = is_long ? 4 : 2;
    if (HOOK_EXISTS_BOUNDED(ctx->uc, UC_HOOK_CODE, ctx->insn_pc)) {
        gen_save_pc(ctx, ctx->insn_pc);
        gen_uc_tracecode(tcg_ctx, size, UC_HOOK_CODE_IDX, ctx->uc,
                         ctx->insn_pc);
        check_exit_request(tcg_ctx);
    }

    ctx->insn = first;
    ctx->next_pc = ctx->insn_pc + size;
    ctx->base.pc_next = ctx->next_pc;
    if (is_long) {
        uint16_t second = cpu_lduw_code(
            cpu->env_ptr, tc32_fetch_address(ctx->env, ctx->insn_pc + 2));
        tc32_translate_32(ctx, second);
    } else {
        tc32_translate_16(ctx);
    }

    if (ctx->base.is_jmp == DISAS_NEXT) {
        target_ulong page_start = ctx->base.pc_first & TARGET_PAGE_MASK;
        if (ctx->base.pc_next - page_start >= TARGET_PAGE_SIZE) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void tc32_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    (void)cpu;

    switch (ctx->base.is_jmp) {
    case DISAS_TOO_MANY:
    case DISAS_UC_EXIT:
        gen_save_pc(ctx, ctx->base.pc_next);
        gen_helper_uc_tc32_exit(ctx->uc->tcg_ctx,
                                ctx->uc->tcg_ctx->cpu_env);
        break;
    case DISAS_NORETURN:
        break;
    default:
        gen_save_pc(ctx, ctx->base.pc_next);
        tcg_gen_exit_tb(ctx->uc->tcg_ctx, NULL, 0);
        break;
    }
}

static void tc32_pc_sync(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    gen_save_pc(ctx, ctx->base.pc_next);
    (void)cpu;
}

static const TranslatorOps tc32_tr_ops = {
    .init_disas_context = tc32_tr_init_disas_context,
    .tb_start = tc32_tr_tb_start,
    .insn_start = tc32_tr_insn_start,
    .translate_insn = tc32_tr_translate_insn,
    .tb_stop = tc32_tr_tb_stop,
    .pc_sync = tc32_pc_sync,
};

void gen_intermediate_code(CPUState *cpu, TranslationBlock *tb, int max_insns)
{
    DisasContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    translator_loop(&tc32_tr_ops, &ctx.base, cpu, tb, max_insns);
}

void restore_state_to_opc(TC32CPUState *env, TranslationBlock *tb,
                          target_ulong *data)
{
    (void)tb;
    env->regs[15] = data[0];
}

void tc32_tcg_init(struct uc_struct *uc)
{
    TCGContext *tcg_ctx = uc->tcg_ctx;
    static const char *const names[16] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
    };

    for (unsigned i = 0; i < 16; i++) {
        tcg_ctx->cpu_R[i] = tcg_global_mem_new_i32(
            tcg_ctx, tcg_ctx->cpu_env,
            offsetof(TC32CPUState, regs[i]), names[i]);
    }
    tcg_ctx->cpu_tc32_cpsr = tcg_global_mem_new_i32(
        tcg_ctx, tcg_ctx->cpu_env, offsetof(TC32CPUState, cpsr), "cpsr");
    tcg_ctx->cpu_tc32_ssr = tcg_global_mem_new_i32(
        tcg_ctx, tcg_ctx->cpu_env, offsetof(TC32CPUState, ssr), "ssr");
}
