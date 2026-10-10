#include "unicorn_test.h"

static const uint64_t code_start = 0x1000;
static const uint64_t code_len = 0x1000;

static void test_tc32_open_and_arithmetic(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x05, 0xa0, /* tmov r0, #5 */
        0x03, 0xb0, /* tadd r0, #3 */
        0x01, 0xb8, /* tsub r0, #1 */
        0x00, 0xc2, /* tjhs +0: branch to the instruction after next */
        0x01, 0xa1, /* skipped: tmov r1, #1 */
        0x02, 0xa1, /* tmov r1, #2 */
    };
    uint32_t r0 = 0;
    uint32_t r1 = 0;
    uint32_t cpsr = 0;
    uint32_t pc = 0;

    TEST_CHECK(uc_arch_supported(UC_ARCH_TC32));
    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));

    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_read(uc, UC_TC32_REG_R1, &r1));
    OK(uc_reg_read(uc, UC_TC32_REG_CPSR, &cpsr));
    OK(uc_reg_read(uc, UC_TC32_REG_PC, &pc));
    TEST_CHECK(r0 == 7);
    TEST_CHECK(r1 == 2);
    TEST_CHECK((cpsr & UC_TC32_FLAG_C) != 0);
    TEST_CHECK(pc == code_start + sizeof(code));
    OK(uc_close(uc));
}

static void test_tc32_pc_literal_load(void)
{
    uc_engine *uc;
    const uint8_t code_and_literal[] = {
        0x00, 0x08, /* tloadr r0, [pc, #0] */
        0x00, 0x00, /* align the literal to (pc + 4) & ~3 */
        0x78, 0x56, 0x34, 0x12,
    };
    uint32_t r0 = 0;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code_and_literal,
                    sizeof(code_and_literal)));
    OK(uc_emu_start(uc, code_start, code_start + 2, 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    TEST_CHECK(r0 == 0x12345678u);
    OK(uc_close(uc));
}

static void test_tc32_stack_and_three_operand_add(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x0c, 0x64, /* tpush {r2, r3} */
        0x00, 0xa2, /* tmov r2, #0 */
        0x00, 0xa3, /* tmov r3, #0 */
        0x0c, 0x6c, /* tpop {r2, r3} */
        0xc2, 0xec, /* tadd r2, r0, #3 */
    };
    uint32_t sp = 0x1800;
    uint32_t r0 = 5;
    uint32_t r2 = 0x22222222;
    uint32_t r3 = 0x33333333;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_TC32_REG_SP, &sp));
    OK(uc_reg_write(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_write(uc, UC_TC32_REG_R2, &r2));
    OK(uc_reg_write(uc, UC_TC32_REG_R3, &r3));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_SP, &sp));
    OK(uc_reg_read(uc, UC_TC32_REG_R2, &r2));
    OK(uc_reg_read(uc, UC_TC32_REG_R3, &r3));
    TEST_CHECK(sp == 0x1800);
    TEST_CHECK(r2 == 8);
    TEST_CHECK(r3 == 0x33333333);
    OK(uc_close(uc));
}

static void test_tc32_add_with_carry_and_shift(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x48, 0x01, /* taddc r0, r1 */
        0x40, 0xf0, /* tshftl r0, r0, #1 */
    };
    uint32_t cpsr = 0x13 | UC_TC32_FLAG_C;
    uint32_t r0 = 0x40000000u;
    uint32_t r1 = 0x3fffffffu;
    uint32_t final_cpsr = 0;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_TC32_REG_CPSR, &cpsr));
    OK(uc_reg_write(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_write(uc, UC_TC32_REG_R1, &r1));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_read(uc, UC_TC32_REG_CPSR, &final_cpsr));
    TEST_CHECK(r0 == 0);
    TEST_CHECK((final_cpsr & UC_TC32_FLAG_C) != 0);
    TEST_CHECK((final_cpsr & UC_TC32_FLAG_Z) != 0);
    OK(uc_close(uc));
}

static void test_tc32_register_shift(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x88, 0x00, /* tshftl r0, r0, r1 */
        0x00, 0x00,
    };
    uint32_t r0 = 0x80000001u;
    uint32_t r1 = 1;
    uint32_t cpsr = 0x13;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_write(uc, UC_TC32_REG_R1, &r1));
    OK(uc_reg_write(uc, UC_TC32_REG_CPSR, &cpsr));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_read(uc, UC_TC32_REG_CPSR, &cpsr));
    TEST_CHECK(r0 == 2);
    TEST_CHECK((cpsr & UC_TC32_FLAG_C) != 0);
    OK(uc_close(uc));
}

static void test_tc32_read_cpsr(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0xcb, 0x6b, /* tmrcs r3 / read CPSR into r3 */
    };
    uint32_t cpsr = 0xd0000013u;
    uint32_t r3 = 0;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_TC32_REG_CPSR, &cpsr));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R3, &r3));
    TEST_CHECK(r3 == cpsr);
    OK(uc_close(uc));
}

static void test_tc32_long_conditional_branch(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x00, 0x90, 0x02, 0x20, /* tjeq to 0x1008 */
        0x01, 0xa0,             /* skipped */
        0x00, 0x00,             /* padding */
        0x02, 0xa0,             /* target: tmov r0, #2 */
    };
    uint32_t cpsr = 0x13 | UC_TC32_FLAG_Z;
    uint32_t r0 = 0;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_TC32_REG_CPSR, &cpsr));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    TEST_CHECK(r0 == 2);
    OK(uc_close(uc));
}

static void test_tc32_short_unconditional_branch(void)
{
    uc_engine *uc;
    uint8_t code[0x406] = {0};
    uint32_t r0 = 0;
    code[0] = 0x00;
    code[1] = 0x8a; /* branch to 0x1404; displacement sets bit 10 */
    code[2] = 0x01;
    code[3] = 0xa0; /* skipped */
    code[0x404] = 0x02;
    code[0x405] = 0xa0; /* target: tmov r0, #2 */

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    TEST_CHECK(r0 == 2);
    OK(uc_close(uc));
}

static void test_tc32_instruction_fetch_alias(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x08, 0x58, /* 0x1000: tloadr r0, [r1, #0] */
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x08, /* 0x1010: tloadr r0, [pc, #0] */
        0x00, 0x00, /* align literal to (pc + 4) & ~3 */
        0x78, 0x56, 0x34, 0x12,
    };
    const uint32_t sram_value = 0x78563412;
    const uint32_t flash_value = 0x11111111;
    uint32_t r0 = 0;
    uint32_t r1 = 0x842000;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, 0, 0x4000, UC_PROT_ALL));
    OK(uc_mem_map(uc, 0x840000, 0x4000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, code, sizeof(code)));
    OK(uc_mem_write(uc, 0x2000, &flash_value, sizeof(flash_value)));
    OK(uc_mem_write(uc, r1, &sram_value, sizeof(sram_value)));
    OK(uc_reg_write(uc, UC_TC32_REG_R1, &r1));
    OK(uc_tc32_set_fetch_alias(uc, 0x840000, 0x4000, 0));

    OK(uc_emu_start(uc, 0x841000, 0x841002, 0, 1));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    TEST_CHECK(r0 == sram_value);

    r0 = 0;
    OK(uc_reg_write(uc, UC_TC32_REG_R0, &r0));
    OK(uc_emu_start(uc, 0x841010, 0x841012, 0, 1));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    TEST_CHECK(r0 == 0x12345678u);
    OK(uc_close(uc));
}

static void test_tc32_long_branches(void)
{
    uc_engine *uc;
    const uint8_t code[] = {
        0x00, 0x90, 0x02, 0x98, /* tjl to 0x1008 */
        0x01, 0xa0,             /* 0x1004: skipped */
        0x00, 0x00,             /* 0x1006: padding */
        0x09, 0xa0,             /* 0x1008: target */
        0x00, 0x00,             /* 0x100a: nop */
    };
    uint32_t r0 = 0;
    uint32_t lr = 0;

    OK(uc_open(UC_ARCH_TC32, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_TC32_REG_R0, &r0));
    OK(uc_reg_read(uc, UC_TC32_REG_LR, &lr));
    TEST_CHECK(r0 == 9);
    TEST_CHECK(lr == code_start + 4);
    OK(uc_close(uc));
}

TEST_LIST = {
    {"tc32_open_and_arithmetic", test_tc32_open_and_arithmetic},
    {"tc32_pc_literal_load", test_tc32_pc_literal_load},
    {"tc32_stack_and_three_operand_add", test_tc32_stack_and_three_operand_add},
    {"tc32_add_with_carry_and_shift", test_tc32_add_with_carry_and_shift},
    {"tc32_register_shift", test_tc32_register_shift},
    {"tc32_read_cpsr", test_tc32_read_cpsr},
    {"tc32_long_branches", test_tc32_long_branches},
    {"tc32_long_conditional_branch", test_tc32_long_conditional_branch},
    {"tc32_short_unconditional_branch", test_tc32_short_unconditional_branch},
    {"tc32_instruction_fetch_alias", test_tc32_instruction_fetch_alias},
    {NULL, NULL},
};
