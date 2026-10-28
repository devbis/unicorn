#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "cpu.h"
#include "uc_priv.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "unicorn/mcs51.h"

#define SFR_ACC 0xe0
#define SFR_B 0xf0
#define SFR_PSW 0xd0
#define SFR_SP 0x81
#define SFR_DPL 0x82
#define SFR_DPH 0x83
#define SFR_P2 0xa0
#define SFR_TCON 0x88
#define SFR_IE 0xa8

/*
 * With a fast memory table installed (UC_MCS51_REG_FAST_MEMORY) register and
 * RAM accesses use the host arrays directly; SFRs flagged special, and every
 * access without a table, go through the soft MMU so mapped callbacks run.
 */
static uint8_t code_read(MCS51CPUState *env, uint16_t addr)
{
    if (env->fast.code) {
        return env->fast.code[addr];
    }
    return cpu_ldub_code(env, addr);
}

static uint8_t fetch8(MCS51CPUState *env, uint32_t *pc)
{
    uint8_t value = code_read(env, (uint16_t)*pc);
    *pc = (*pc + 1u) & 0xffffu;
    return value;
}

static uint8_t direct_read(MCS51CPUState *env, uint8_t addr)
{
    if (env->fast.idata) {
        if (addr < 0x80) {
            return env->fast.idata[addr];
        }
        if (!env->fast.sfr_special_read[addr]) {
            return env->fast.sfr[addr];
        }
    }
    abi_ptr space = addr < 0x80 ? UC_MCS51_IDATA_BASE : UC_MCS51_SFR_BASE;
    return cpu_ldub_data(env, space + addr);
}

static void direct_write(MCS51CPUState *env, uint8_t addr, uint8_t value)
{
    if (env->fast.idata) {
        if (addr < 0x80) {
            env->fast.idata[addr] = value;
            return;
        }
        if (!env->fast.sfr_special_write[addr]) {
            env->fast.sfr[addr] = value;
            return;
        }
    }
    abi_ptr space = addr < 0x80 ? UC_MCS51_IDATA_BASE : UC_MCS51_SFR_BASE;
    cpu_stb_data(env, space + addr, value);
}

static uint8_t indirect_read(MCS51CPUState *env, uint8_t addr)
{
    if (env->fast.idata) {
        return env->fast.idata[addr];
    }
    return cpu_ldub_data(env, UC_MCS51_IDATA_BASE + addr);
}

/* The destination read of a read-modify-write instruction (see
   UC_MCS51_REG_RMW_ACCESS). */
static uint8_t direct_read_rmw(MCS51CPUState *env, uint8_t addr)
{
    uint8_t value;
    env->rmw_access = 1;
    value = direct_read(env, addr);
    env->rmw_access = 0;
    return value;
}

static void indirect_write(MCS51CPUState *env, uint8_t addr, uint8_t value)
{
    if (env->fast.idata) {
        env->fast.idata[addr] = value;
        return;
    }
    cpu_stb_data(env, UC_MCS51_IDATA_BASE + addr, value);
}

static uint8_t reg_read(MCS51CPUState *env, unsigned reg)
{
    uint8_t bank = (direct_read(env, SFR_PSW) >> 3) & 3u;
    return indirect_read(env, (uint8_t)(bank * 8u + (reg & 7u)));
}

static void reg_write(MCS51CPUState *env, unsigned reg, uint8_t value)
{
    uint8_t bank = (direct_read(env, SFR_PSW) >> 3) & 3u;
    indirect_write(env, (uint8_t)(bank * 8u + (reg & 7u)), value);
}

static uint8_t acc_read(MCS51CPUState *env)
{
    return direct_read(env, SFR_ACC);
}

static void acc_write(MCS51CPUState *env, uint8_t value)
{
    direct_write(env, SFR_ACC, value);
}

/*
 * The data pointer selected for DPTR instructions: DPL/DPH, or the second
 * pointer when one is configured (UC_MCS51_REG_DPS_SFR) and DPS bit 0 is 1.
 */
static void dptr_select(MCS51CPUState *env, uint8_t *low, uint8_t *high)
{
    if (env->dps_sfr && env->dpl1_sfr && env->dph1_sfr &&
        (direct_read(env, env->dps_sfr) & 1u)) {
        *low = env->dpl1_sfr;
        *high = env->dph1_sfr;
    } else {
        *low = SFR_DPL;
        *high = SFR_DPH;
    }
}

static uint16_t dptr_read(MCS51CPUState *env)
{
    uint8_t low, high;
    dptr_select(env, &low, &high);
    return (uint16_t)(direct_read(env, low) |
                      ((uint16_t)direct_read(env, high) << 8));
}

static void dptr_write(MCS51CPUState *env, uint16_t value)
{
    uint8_t low, high;
    dptr_select(env, &low, &high);
    direct_write(env, low, (uint8_t)value);
    direct_write(env, high, (uint8_t)(value >> 8));
}

/* Highest set bit of a nonzero level mask. */
static unsigned highest_level(uint8_t levels)
{
    unsigned level = 7;
    while (!(levels & (1u << level))) {
        level--;
    }
    return level;
}

static bool carry_read(MCS51CPUState *env)
{
    return (direct_read(env, SFR_PSW) & UC_MCS51_FLAG_CY) != 0;
}

static void carry_write(MCS51CPUState *env, bool value)
{
    uint8_t psw = direct_read(env, SFR_PSW);
    psw = value ? (uint8_t)(psw | UC_MCS51_FLAG_CY)
                : (uint8_t)(psw & ~UC_MCS51_FLAG_CY);
    direct_write(env, SFR_PSW, psw);
}

static bool bit_read(MCS51CPUState *env, uint8_t bitaddr)
{
    uint8_t byteaddr;
    uint8_t bit;
    if (bitaddr < 0x80) {
        byteaddr = (uint8_t)(0x20u + (bitaddr >> 3));
        bit = bitaddr & 7u;
    } else {
        byteaddr = bitaddr & 0xf8u;
        bit = bitaddr & 7u;
    }
    return (direct_read(env, byteaddr) & (1u << bit)) != 0;
}

static bool bit_read_rmw(MCS51CPUState *env, uint8_t bitaddr)
{
    bool value;
    env->rmw_access = 1;
    value = bit_read(env, bitaddr);
    env->rmw_access = 0;
    return value;
}

static void bit_write(MCS51CPUState *env, uint8_t bitaddr, bool value)
{
    uint8_t byteaddr;
    uint8_t bit;
    uint8_t byte;
    if (bitaddr < 0x80) {
        byteaddr = (uint8_t)(0x20u + (bitaddr >> 3));
        bit = bitaddr & 7u;
    } else {
        byteaddr = bitaddr & 0xf8u;
        bit = bitaddr & 7u;
    }
    byte = direct_read_rmw(env, byteaddr);
    byte = value ? (uint8_t)(byte | (1u << bit))
                 : (uint8_t)(byte & ~(1u << bit));
    direct_write(env, byteaddr, byte);
}

static void set_parity(MCS51CPUState *env)
{
    uint8_t psw = direct_read(env, SFR_PSW);
    uint8_t acc = acc_read(env);
#ifdef _MSC_VER
    const int odd = ctpop8(acc) & 1;
#else
    const int odd = __builtin_parity((unsigned)acc);
#endif
    psw = (uint8_t)((psw & ~UC_MCS51_FLAG_P) | (odd ? UC_MCS51_FLAG_P : 0));
    direct_write(env, SFR_PSW, psw);
}

static uint8_t read_a_source(MCS51CPUState *env, uint8_t opcode,
                             uint32_t *pc)
{
    switch (opcode & 0x0fu) {
    case 4:
        return fetch8(env, pc);
    case 5:
        return direct_read(env, fetch8(env, pc));
    case 6:
    case 7: {
        uint8_t address = reg_read(env, opcode & 1u);
        return indirect_read(env, address);
    }
    default:
        return reg_read(env, opcode & 7u);
    }
}

static void alu_add(MCS51CPUState *env, uint8_t rhs, bool with_carry)
{
    uint8_t lhs = acc_read(env);
    uint8_t psw = direct_read(env, SFR_PSW);
    unsigned carry = with_carry && (psw & UC_MCS51_FLAG_CY) != 0;
    unsigned sum = (unsigned)lhs + rhs + carry;
    uint8_t result = (uint8_t)sum;
    psw &= (uint8_t)~(UC_MCS51_FLAG_CY | UC_MCS51_FLAG_AC | UC_MCS51_FLAG_OV);
    if (sum > 0xffu)
        psw |= UC_MCS51_FLAG_CY;
    if (((lhs & 0x0fu) + (rhs & 0x0fu) + carry) > 0x0fu)
        psw |= UC_MCS51_FLAG_AC;
    if ((~(lhs ^ rhs) & (lhs ^ result) & 0x80u) != 0)
        psw |= UC_MCS51_FLAG_OV;
    direct_write(env, SFR_PSW, psw);
    acc_write(env, result);
}

static void alu_subb(MCS51CPUState *env, uint8_t rhs)
{
    uint8_t lhs = acc_read(env);
    uint8_t psw = direct_read(env, SFR_PSW);
    unsigned borrow = (psw & UC_MCS51_FLAG_CY) != 0;
    unsigned subtrahend = (unsigned)rhs + borrow;
    uint8_t result = (uint8_t)((unsigned)lhs - subtrahend);
    psw &= (uint8_t)~(UC_MCS51_FLAG_CY | UC_MCS51_FLAG_AC | UC_MCS51_FLAG_OV);
    if ((unsigned)lhs < subtrahend)
        psw |= UC_MCS51_FLAG_CY;
    if ((lhs & 0x0fu) < ((rhs & 0x0fu) + borrow))
        psw |= UC_MCS51_FLAG_AC;
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80u) != 0)
        psw |= UC_MCS51_FLAG_OV;
    direct_write(env, SFR_PSW, psw);
    acc_write(env, result);
}

static void stack_push(MCS51CPUState *env, uint8_t value)
{
    uint8_t sp = (uint8_t)(direct_read(env, SFR_SP) + 1u);
    direct_write(env, SFR_SP, sp);
    indirect_write(env, sp, value);
}

static uint8_t stack_pop(MCS51CPUState *env)
{
    uint8_t sp = direct_read(env, SFR_SP);
    uint8_t value = indirect_read(env, sp);
    direct_write(env, SFR_SP, (uint8_t)(sp - 1u));
    return value;
}

static uint16_t fetch16(MCS51CPUState *env, uint32_t *pc)
{
    uint8_t high = fetch8(env, pc);
    uint8_t low = fetch8(env, pc);
    return (uint16_t)(((uint16_t)high << 8) | low);
}

static void push_return_pc(MCS51CPUState *env, uint16_t pc)
{
    stack_push(env, (uint8_t)pc);
    stack_push(env, (uint8_t)(pc >> 8));
}

static void invalid_instruction(MCS51CPUState *env, uint16_t pc)
{
    env->pc = pc;
    env->uc->invalid_error = UC_ERR_INSN_INVALID;
    env->uc->cpu->exception_index = EXCP_HLT;
    env->uc->cpu->halted = 1;
    cpu_loop_exit(env->uc->cpu);
}

hwaddr mcs51_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr)
{
    (void)cpu;
    return addr;
}

bool mcs51_cpu_tlb_fill(CPUState *cpu, vaddr address, int size,
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

void helper_uc_mcs51_exit(MCS51CPUState *env)
{
    CPUState *cpu = env->uc->cpu;
    cpu->exception_index = EXCP_HLT;
    cpu->halted = 1;
    cpu_loop_exit(cpu);
}

/*
 * UC_MCS51_CYCLE_MODEL_CC253X: CPU clock cycles per opcode, the "Cycles"
 * column of TI SWRU191F (CC253x/CC2540/CC2541 user's guide) Table 2-3,
 * Instruction Set Summary. The note above the table makes these best-case
 * counts (single-cycle access to the memory operand); flash and XDATA wait
 * states are not modelled. The table gives one count per conditional
 * branch, used whether or not it jumps. 0xa5 is reserved (rejected before
 * any cycle is charged).
 */
static const uint8_t cc253x_cycles[256] = {
    /*      x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xa xb xc xd xe xf */
    /* 0x */ 1, 3, 4, 1, 1, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 1x */ 4, 6, 6, 1, 1, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 2x */ 4, 3, 4, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 3x */ 4, 6, 4, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 4x */ 3, 3, 3, 4, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 5x */ 3, 6, 3, 4, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 6x */ 3, 3, 3, 4, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 7x */ 3, 6, 2, 2, 2, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 8x */ 3, 3, 2, 3, 5, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3,
    /* 9x */ 3, 6, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* ax */ 2, 3, 2, 1, 5, 0, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
    /* bx */ 2, 6, 3, 1, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    /* cx */ 4, 3, 3, 1, 1, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    /* dx */ 3, 6, 3, 1, 1, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    /* ex */ 3, 3, 3, 3, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    /* fx */ 4, 6, 4, 4, 1, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
};

/* Cycles charged for an interrupt entry, which happens at an instruction
   boundary (a request arriving during an instruction waits for its end). */
static unsigned interrupt_entry_cycles(const MCS51CPUState *env)
{
    if (env->cycle_model == UC_MCS51_CYCLE_MODEL_CC253X) {
        /* SWRU191F 2.5.2: the fastest response is seven machine cycles,
           one to detect the request and six for the forced LCALL. */
        return 7u;
    }
    return 2u;
}

static void mcs51_execute_one(MCS51CPUState *env)
{
    uint16_t instruction_pc = (uint16_t)env->pc;
    uint32_t pc = instruction_pc;
    uint8_t opcode = fetch8(env, &pc);
    uint8_t a;
    uint8_t value;
    uint8_t operand;
    uint8_t address;
    uint8_t bitaddr;
    uint8_t rel;
    uint16_t target;
    unsigned cycles = 1;
    bool take;

    env->instructions++;

    if (env->interrupt_inhibit) {
        /* The instruction after RETI executes before any interrupt. */
        env->interrupt_inhibit = 0;
    } else if (env->timer0_interrupt && !env->interrupt_levels &&
               (direct_read(env, SFR_IE) & 0x82u) == 0x82u &&
               (direct_read(env, SFR_TCON) & 0x20u) != 0) {
        /* Timer 0 uses the standard 8051 vector at 0x000b. */
        push_return_pc(env, instruction_pc);
        direct_write(env, SFR_TCON,
                     (uint8_t)(direct_read(env, SFR_TCON) & ~0x20u));
        env->interrupt_levels = 1;
        env->pc = 0x000bu;
        env->cycles += interrupt_entry_cycles(env);
        return;
    } else if (env->interrupt_vector != 0 &&
               (!env->interrupt_levels ||
                env->interrupt_level > highest_level(env->interrupt_levels))) {
        /* SoC models request their part-specific vector and its level; only
           a higher level than every one in progress preempts. */
        uint16_t vector = env->interrupt_vector;
        env->interrupt_vector = 0;
        push_return_pc(env, instruction_pc);
        env->interrupt_levels |= (uint8_t)(1u << env->interrupt_level);
        env->pc = vector;
        env->cycles += interrupt_entry_cycles(env);
        return;
    }

    /* AJMP and ACALL encode the upper three target bits in the opcode. */
    if ((opcode & 0x1fu) == 0x01u) {
        uint8_t low = fetch8(env, &pc);
        target = (uint16_t)((pc & 0xf800u) | ((opcode & 0xe0u) << 3) | low);
        pc = target;
        cycles = 2;
    } else if ((opcode & 0x1fu) == 0x11u) {
        uint8_t low = fetch8(env, &pc);
        target = (uint16_t)((pc & 0xf800u) | ((opcode & 0xe0u) << 3) | low);
        push_return_pc(env, (uint16_t)pc);
        pc = target;
        cycles = 2;
    } else if ((opcode >= 0x24u && opcode <= 0x2fu) ||
               (opcode >= 0x34u && opcode <= 0x3fu)) {
        alu_add(env, read_a_source(env, opcode, &pc), opcode >= 0x34u);
    } else if (opcode >= 0x94u && opcode <= 0x9fu) {
        alu_subb(env, read_a_source(env, opcode, &pc));
    } else if ((opcode >= 0x44u && opcode <= 0x4fu) ||
               (opcode >= 0x54u && opcode <= 0x5fu) ||
               (opcode >= 0x64u && opcode <= 0x6fu)) {
        value = read_a_source(env, opcode, &pc);
        a = acc_read(env);
        if ((opcode & 0xf0u) == 0x40u)
            acc_write(env, (uint8_t)(a | value));
        else if ((opcode & 0xf0u) == 0x50u)
            acc_write(env, (uint8_t)(a & value));
        else
            acc_write(env, (uint8_t)(a ^ value));
    } else if (opcode >= 0x08u && opcode <= 0x0fu) {
        reg_write(env, opcode & 7u, (uint8_t)(reg_read(env, opcode & 7u) + 1u));
    } else if (opcode >= 0x18u && opcode <= 0x1fu) {
        reg_write(env, opcode & 7u, (uint8_t)(reg_read(env, opcode & 7u) - 1u));
    } else if (opcode >= 0x28u && opcode <= 0x2fu) {
        /* Handled by the arithmetic group above. */
    } else if (opcode >= 0x38u && opcode <= 0x3fu) {
        /* Handled by the arithmetic group above. */
    } else if (opcode >= 0x78u && opcode <= 0x7fu) {
        reg_write(env, opcode & 7u, fetch8(env, &pc));
    } else if (opcode >= 0x88u && opcode <= 0x8fu) {
        direct_write(env, fetch8(env, &pc), reg_read(env, opcode & 7u));
    } else if (opcode >= 0x98u && opcode <= 0x9fu) {
        /* Handled by SUBB A,Rn above. */
    } else if (opcode >= 0xa8u && opcode <= 0xafu) {
        reg_write(env, opcode & 7u, direct_read(env, fetch8(env, &pc)));
    } else if (opcode >= 0xb8u && opcode <= 0xbfu) {
        address = reg_read(env, opcode & 7u);
        operand = fetch8(env, &pc);
        rel = fetch8(env, &pc);
        carry_write(env, address < operand);
        if (address != operand)
            pc = (uint16_t)(pc + (int8_t)rel);
        cycles = 2;
    } else if (opcode >= 0xc8u && opcode <= 0xcfu) {
        value = reg_read(env, opcode & 7u);
        reg_write(env, opcode & 7u, acc_read(env));
        acc_write(env, value);
    } else if (opcode >= 0xd8u && opcode <= 0xdfu) {
        rel = fetch8(env, &pc);
        value = (uint8_t)(reg_read(env, opcode & 7u) - 1u);
        reg_write(env, opcode & 7u, value);
        if (value != 0)
            pc = (uint16_t)(pc + (int8_t)rel);
        cycles = 2;
    } else if (opcode >= 0xe8u && opcode <= 0xefu) {
        acc_write(env, reg_read(env, opcode & 7u));
    } else if (opcode >= 0xf8u && opcode <= 0xffu) {
        reg_write(env, opcode & 7u, acc_read(env));
    } else {
        switch (opcode) {
        case 0x00: /* NOP */
            break;
        case 0x02: /* LJMP addr16 */
            pc = fetch16(env, &pc);
            cycles = 2;
            break;
        case 0x03: /* RR A */
            a = acc_read(env);
            acc_write(env, (uint8_t)((a >> 1) | (a << 7)));
            break;
        case 0x04: /* INC A */
            acc_write(env, (uint8_t)(acc_read(env) + 1u));
            break;
        case 0x05: /* INC direct */
            address = fetch8(env, &pc);
            direct_write(env, address, (uint8_t)(direct_read_rmw(env, address) + 1u));
            cycles = 1;
            break;
        case 0x06: case 0x07: /* INC @Ri */
            address = reg_read(env, opcode & 1u);
            indirect_write(env, address, (uint8_t)(indirect_read(env, address) + 1u));
            break;
        case 0x10: /* JBC bit,rel */
            bitaddr = fetch8(env, &pc);
            rel = fetch8(env, &pc);
            take = bit_read_rmw(env, bitaddr);
            if (take) {
                bit_write(env, bitaddr, false);
                pc = (uint16_t)(pc + (int8_t)rel);
            }
            cycles = 2;
            break;
        case 0x12: /* LCALL addr16 */
            target = fetch16(env, &pc);
            push_return_pc(env, (uint16_t)pc);
            pc = target;
            cycles = 2;
            break;
        case 0x13: /* RRC A */
            a = acc_read(env);
            value = carry_read(env) ? 0x80u : 0;
            carry_write(env, (a & 1u) != 0);
            acc_write(env, (uint8_t)((a >> 1) | value));
            break;
        case 0x14: /* DEC A */
            acc_write(env, (uint8_t)(acc_read(env) - 1u));
            break;
        case 0x15: /* DEC direct */
            address = fetch8(env, &pc);
            direct_write(env, address, (uint8_t)(direct_read_rmw(env, address) - 1u));
            break;
        case 0x16: case 0x17: /* DEC @Ri */
            address = reg_read(env, opcode & 1u);
            indirect_write(env, address, (uint8_t)(indirect_read(env, address) - 1u));
            break;
        case 0x20: case 0x30: /* JB/JNB bit,rel */
            bitaddr = fetch8(env, &pc);
            rel = fetch8(env, &pc);
            take = bit_read(env, bitaddr) == (opcode == 0x20);
            if (take)
                pc = (uint16_t)(pc + (int8_t)rel);
            cycles = 2;
            break;
        case 0x22: /* RET */
            value = stack_pop(env);
            address = stack_pop(env);
            pc = (uint16_t)(((uint16_t)value << 8) | address);
            cycles = 2;
            break;
        case 0x23: /* RL A */
            a = acc_read(env);
            acc_write(env, (uint8_t)((a << 1) | (a >> 7)));
            break;
        case 0x32: /* RETI */
            value = stack_pop(env);
            address = stack_pop(env);
            pc = (uint16_t)(((uint16_t)value << 8) | address);
            if (env->interrupt_levels) {
                env->interrupt_levels &=
                    (uint8_t)~(1u << highest_level(env->interrupt_levels));
            }
            if (env->post_reti_rule) {
                env->interrupt_inhibit = 1;
            }
            cycles = 2;
            break;
        case 0x33: /* RLC A */
            a = acc_read(env);
            value = carry_read(env) ? 1u : 0;
            carry_write(env, (a & 0x80u) != 0);
            acc_write(env, (uint8_t)((a << 1) | value));
            break;
        case 0x40: case 0x50: /* JC/JNC rel */
            rel = fetch8(env, &pc);
            if (carry_read(env) == (opcode == 0x40))
                pc = (uint16_t)(pc + (int8_t)rel);
            cycles = 2;
            break;
        case 0x42: case 0x52: case 0x62: /* ORL/ANL/XRL direct,A */
            address = fetch8(env, &pc);
            value = direct_read_rmw(env, address);
            a = acc_read(env);
            if (opcode == 0x42)
                value |= a;
            else if (opcode == 0x52)
                value &= a;
            else
                value ^= a;
            direct_write(env, address, value);
            break;
        case 0x43: case 0x53: case 0x63: /* ORL/ANL/XRL direct,#data */
            address = fetch8(env, &pc);
            operand = fetch8(env, &pc);
            value = direct_read_rmw(env, address);
            if (opcode == 0x43)
                value |= operand;
            else if (opcode == 0x53)
                value &= operand;
            else
                value ^= operand;
            direct_write(env, address, value);
            cycles = 2;
            break;
        case 0x60: case 0x70: /* JZ/JNZ rel */
            rel = fetch8(env, &pc);
            a = acc_read(env);
            if ((a == 0) == (opcode == 0x60))
                pc = (uint16_t)(pc + (int8_t)rel);
            cycles = 2;
            break;
        case 0x72: case 0x82: /* ORL/ANL C,bit */
            bitaddr = fetch8(env, &pc);
            if (opcode == 0x72)
                carry_write(env, carry_read(env) || bit_read(env, bitaddr));
            else
                carry_write(env, carry_read(env) && bit_read(env, bitaddr));
            break;
        case 0x73: /* JMP @A+DPTR */
            pc = (uint16_t)(dptr_read(env) + acc_read(env));
            cycles = 2;
            break;
        case 0x74: /* MOV A,#data */
            acc_write(env, fetch8(env, &pc));
            break;
        case 0x75: /* MOV direct,#data */
            address = fetch8(env, &pc);
            direct_write(env, address, fetch8(env, &pc));
            cycles = 2;
            break;
        case 0x76: case 0x77: /* MOV @Ri,#data */
            address = reg_read(env, opcode & 1u);
            indirect_write(env, address, fetch8(env, &pc));
            break;
        case 0x80: /* SJMP rel */
            rel = fetch8(env, &pc);
            pc = (uint16_t)(pc + (int8_t)rel);
            cycles = 2;
            break;
        case 0x83: /* MOVC A,@A+PC */
            a = acc_read(env);
            acc_write(env, code_read(env, (uint16_t)(pc + a)));
            cycles = 2;
            break;
        case 0x84: /* DIV AB */
            a = acc_read(env);
            value = direct_read(env, SFR_B);
            operand = direct_read(env, SFR_PSW);
            operand &= (uint8_t)~(UC_MCS51_FLAG_CY | UC_MCS51_FLAG_OV);
            if (value == 0) {
                operand |= UC_MCS51_FLAG_OV;
            } else {
                acc_write(env, (uint8_t)(a / value));
                direct_write(env, SFR_B, (uint8_t)(a % value));
            }
            direct_write(env, SFR_PSW, operand);
            cycles = 4;
            break;
        case 0x85: /* MOV direct,direct (source byte precedes destination) */
            operand = fetch8(env, &pc);
            address = fetch8(env, &pc);
            direct_write(env, address, direct_read(env, operand));
            cycles = 2;
            break;
        case 0x86: case 0x87: /* MOV direct,@Ri */
            address = fetch8(env, &pc);
            direct_write(env, address, indirect_read(env, reg_read(env, opcode & 1u)));
            cycles = 2;
            break;
        case 0x90: /* MOV DPTR,#data16 */
            dptr_write(env, fetch16(env, &pc));
            cycles = 2;
            break;
        case 0x92: /* MOV bit,C */
            bit_write(env, fetch8(env, &pc), carry_read(env));
            break;
        case 0x93: /* MOVC A,@A+DPTR */
            acc_write(env, code_read(env, (uint16_t)(dptr_read(env) + acc_read(env))));
            cycles = 2;
            break;
        case 0xa0: case 0xb0: /* ORL/ANL C,/bit */
            bitaddr = fetch8(env, &pc);
            value = bit_read(env, bitaddr) ? 0 : 1;
            if (opcode == 0xa0)
                carry_write(env, carry_read(env) || value);
            else
                carry_write(env, carry_read(env) && value);
            break;
        case 0xa2: /* MOV C,bit */
            carry_write(env, bit_read(env, fetch8(env, &pc)));
            break;
        case 0xa3: /* INC DPTR */
            dptr_write(env, (uint16_t)(dptr_read(env) + 1u));
            cycles = 2;
            break;
        case 0xa4: /* MUL AB */
            target = (uint16_t)(acc_read(env) * direct_read(env, SFR_B));
            acc_write(env, (uint8_t)target);
            direct_write(env, SFR_B, (uint8_t)(target >> 8));
            value = direct_read(env, SFR_PSW);
            value &= (uint8_t)~(UC_MCS51_FLAG_CY | UC_MCS51_FLAG_OV);
            if ((target >> 8) != 0)
                value |= UC_MCS51_FLAG_OV;
            direct_write(env, SFR_PSW, value);
            cycles = 4;
            break;
        case 0xa6: case 0xa7: /* MOV @Ri,direct */
            value = direct_read(env, fetch8(env, &pc));
            indirect_write(env, reg_read(env, opcode & 1u), value);
            cycles = 2;
            break;
        case 0xb2: /* CPL bit */
            bitaddr = fetch8(env, &pc);
            bit_write(env, bitaddr, !bit_read_rmw(env, bitaddr));
            break;
        case 0xb3: /* CPL C */
            carry_write(env, !carry_read(env));
            break;
        case 0xb4: case 0xb5: case 0xb6: case 0xb7: /* CJNE A/direct/@Ri */
            if (opcode == 0xb4) {
                a = acc_read(env);
                operand = fetch8(env, &pc);
            } else if (opcode == 0xb5) {
                a = acc_read(env);
                operand = direct_read(env, fetch8(env, &pc));
            } else {
                a = indirect_read(env, reg_read(env, opcode & 1u));
                operand = fetch8(env, &pc);
            }
            rel = fetch8(env, &pc);
            carry_write(env, a < operand);
            if (a != operand)
                pc = (uint16_t)(pc + (int8_t)rel);
            cycles = 2;
            break;
        case 0xc0: /* PUSH direct */
            stack_push(env, direct_read(env, fetch8(env, &pc)));
            cycles = 2;
            break;
        case 0xc2: /* CLR bit */
            bit_write(env, fetch8(env, &pc), false);
            break;
        case 0xc3: /* CLR C */
            carry_write(env, false);
            break;
        case 0xc4: /* SWAP A */
            a = acc_read(env);
            acc_write(env, (uint8_t)((a << 4) | (a >> 4)));
            break;
        case 0xc5: /* XCH A,direct */
            address = fetch8(env, &pc);
            value = direct_read(env, address);
            direct_write(env, address, acc_read(env));
            acc_write(env, value);
            break;
        case 0xc6: case 0xc7: /* XCH A,@Ri */
            address = reg_read(env, opcode & 1u);
            value = indirect_read(env, address);
            indirect_write(env, address, acc_read(env));
            acc_write(env, value);
            break;
        case 0xd0: /* POP direct */
            value = stack_pop(env);
            direct_write(env, fetch8(env, &pc), value);
            cycles = 2;
            break;
        case 0xd2: /* SETB bit */
            bit_write(env, fetch8(env, &pc), true);
            break;
        case 0xd3: /* SETB C */
            carry_write(env, true);
            break;
        case 0xd4: /* DA A */
            a = acc_read(env);
            value = direct_read(env, SFR_PSW);
            take = (value & UC_MCS51_FLAG_CY) != 0;
            operand = 0;
            if ((a & 0x0fu) > 9u || (value & UC_MCS51_FLAG_AC))
                operand = 0x06;
            if (a > 0x99u || (value & UC_MCS51_FLAG_CY)) {
                operand |= 0x60;
                take = true;
            }
            acc_write(env, (uint8_t)(a + operand));
            value = (uint8_t)((value & ~UC_MCS51_FLAG_CY) |
                              (take || (a + operand) > 0xffu
                                   ? UC_MCS51_FLAG_CY
                                   : 0));
            direct_write(env, SFR_PSW, value);
            break;
        case 0xd5: /* DJNZ direct,rel */
            address = fetch8(env, &pc);
            rel = fetch8(env, &pc);
            value = (uint8_t)(direct_read_rmw(env, address) - 1u);
            direct_write(env, address, value);
            if (value != 0)
                pc = (uint16_t)(pc + (int8_t)rel);
            cycles = 2;
            break;
        case 0xd6: case 0xd7: /* XCHD A,@Ri */
            address = reg_read(env, opcode & 1u);
            value = indirect_read(env, address);
            a = acc_read(env);
            indirect_write(env, address, (uint8_t)((value & 0xf0u) | (a & 0x0fu)));
            acc_write(env, (uint8_t)((a & 0xf0u) | (value & 0x0fu)));
            break;
        case 0xe0: /* MOVX A,@DPTR */
            acc_write(env, cpu_ldub_data(env, UC_MCS51_XDATA_BASE + dptr_read(env)));
            cycles = 2;
            break;
        case 0xe2: case 0xe3: /* MOVX A,@Ri */
            address = reg_read(env, opcode & 1u);
            value = direct_read(env, env->xdata_page_sfr);
            acc_write(env, cpu_ldub_data(env, UC_MCS51_XDATA_BASE +
                                                   ((uint16_t)value << 8) + address));
            cycles = 2;
            break;
        case 0xe4: /* CLR A */
            acc_write(env, 0);
            break;
        case 0xe5: /* MOV A,direct */
            acc_write(env, direct_read(env, fetch8(env, &pc)));
            break;
        case 0xe6: case 0xe7: /* MOV A,@Ri */
            acc_write(env, indirect_read(env, reg_read(env, opcode & 1u)));
            break;
        case 0xf0: /* MOVX @DPTR,A */
            cpu_stb_data(env, UC_MCS51_XDATA_BASE + dptr_read(env), acc_read(env));
            cycles = 2;
            break;
        case 0xf2: case 0xf3: /* MOVX @Ri,A */
            address = reg_read(env, opcode & 1u);
            value = direct_read(env, env->xdata_page_sfr);
            cpu_stb_data(env, UC_MCS51_XDATA_BASE + ((uint16_t)value << 8) + address,
                         acc_read(env));
            cycles = 2;
            break;
        case 0xf4: /* CPL A */
            acc_write(env, (uint8_t)~acc_read(env));
            break;
        case 0xf5: /* MOV direct,A */
            direct_write(env, fetch8(env, &pc), acc_read(env));
            break;
        case 0xf6: case 0xf7: /* MOV @Ri,A */
            indirect_write(env, reg_read(env, opcode & 1u), acc_read(env));
            break;
        default:
            invalid_instruction(env, instruction_pc);
            return;
        }
    }

    if ((opcode >= 0xb4 && opcode <= 0xb7) ||
        (opcode >= 0xb8 && opcode <= 0xbf) ||
        opcode == 0x10 || opcode == 0x20 || opcode == 0x30 ||
        opcode == 0x40 || opcode == 0x50 || opcode == 0x60 ||
        opcode == 0x70 || opcode == 0x80 || opcode == 0xd5 ||
        (opcode >= 0xd8 && opcode <= 0xdf))
        cycles = 2;

    if (env->cycle_model == UC_MCS51_CYCLE_MODEL_CC253X) {
        cycles = cc253x_cycles[opcode];
    }

    env->pc = pc & 0xffffu;
    env->cycles += cycles;
    set_parity(env);
}

void helper_uc_mcs51_step(MCS51CPUState *env)
{
    mcs51_execute_one(env);
}

/*
 * Runs instructions until the host needs control (see unicorn/mcs51.h), then
 * stops the emulation so uc_emu_start returns. The first instruction always
 * executes, so the host can service whatever caused the previous stop.
 */
void helper_uc_mcs51_run(MCS51CPUState *env)
{
    struct uc_struct *uc = env->uc;

    for (;;) {
        mcs51_execute_one(env);
        if (uc->stop_request || env->cycles >= env->event_cycles ||
            env->instructions >= env->instruction_limit ||
            (uint32_t)env->pc == env->break_pc) {
            break;
        }
    }
    uc_emu_stop((uc_engine *)uc);
}
