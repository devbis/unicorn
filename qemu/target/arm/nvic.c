/*
 * ARMv7-M NVIC and System Control Space, for Unicorn.
 *
 * QEMU models the NVIC as a qdev device (hw/intc/armv7m_nvic.c), which a
 * CPU-only engine does not have. This is a self-contained replacement for the
 * single-security-state ARMv7-M case (Cortex-M3/M4): the exception priority
 * logic follows QEMU's, the register file is the System Control Space that
 * firmware sees at 0xE000E000. SysTick is not modelled here: a host owns the
 * time base and raises the SysTick exception with armv7m_nvic_set_irq().
 *
 * The host reaches this through uc_arm_nvic() (see include/unicorn/arm.h).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "sysemu/cpus.h"

#define NVIC_FIRST_IRQ 16
#define NVIC_MAX_VECTORS 512
#define NVIC_NOEXC_PRIO 0x100

typedef struct NvicVector {
    uint8_t prio;
    bool enabled;
    bool pending;
    bool active;
    bool level; /* state of the interrupt line (external IRQs) */
} NvicVector;

typedef struct NVICState {
    ARMCPU *cpu;
    NvicVector vec[NVIC_MAX_VECTORS];
    int num_vectors;   /* including the 16 system exceptions */
    int prio_bits;     /* implemented priority bits, counted from bit 7 */
    int vectpending;   /* highest-priority enabled pending exception, 0 if none */
    int vectpending_prio;
    int exception_prio; /* group priority of the highest-priority active one */
    uint32_t shcsr_pend; /* SHCSR *PENDED bits that firmware wrote */
    bool reset_requested;
} NVICState;

static CPUARMState *nvic_env(NVICState *s)
{
    return &s->cpu->env;
}

static int nvic_prigroup(NVICState *s)
{
    return extract32(nvic_env(s)->v7m.aircr, R_V7M_AIRCR_PRIGROUP_SHIFT, 3);
}

static int nvic_raw_prio(NVICState *s, int irq)
{
    switch (irq) {
    case ARMV7M_EXCP_RESET:
        return -3;
    case ARMV7M_EXCP_NMI:
        return -2;
    case ARMV7M_EXCP_HARD:
        return -1;
    default:
        return s->vec[irq].prio;
    }
}

/* Group priority: the sub-priority bits do not take part in preemption. */
static int nvic_group_prio(NVICState *s, int raw)
{
    if (raw < 0) {
        return raw;
    }
    return raw & (~0u << (nvic_prigroup(s) + 1)) & 0xff;
}

static int nvic_exec_prio(NVICState *s)
{
    CPUARMState *env = nvic_env(s);
    int running = NVIC_NOEXC_PRIO;
    uint32_t basepri = env->v7m.basepri[M_REG_NS];

    if (env->v7m.faultmask[M_REG_NS]) {
        running = -1;
    } else if (env->v7m.primask[M_REG_NS]) {
        running = 0;
    }
    if (basepri) {
        int p = nvic_group_prio(s, basepri & 0xff);
        if (p < running) {
            running = p;
        }
    }
    if (s->exception_prio < running) {
        running = s->exception_prio;
    }
    return running;
}

static void nvic_irq_update(NVICState *s)
{
    CPUState *cs = CPU(s->cpu);

    /*
     * Keep the CPU's interrupt line up whenever something is pending: whether
     * it can be taken (BASEPRI, PRIMASK and so on) is decided at the point of
     * taking it, so unmasking needs no extra notification.
     */
    if (s->vectpending) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void nvic_recompute_state(NVICState *s)
{
    int active_prio = NVIC_NOEXC_PRIO;
    int pend_prio = NVIC_NOEXC_PRIO;
    int pend_irq = 0;
    int i;

    for (i = 1; i < s->num_vectors; i++) {
        NvicVector *v = &s->vec[i];
        int prio = nvic_group_prio(s, nvic_raw_prio(s, i));

        if (v->enabled && v->pending && prio < pend_prio) {
            pend_prio = prio;
            pend_irq = i;
        }
        if (v->active && prio < active_prio) {
            active_prio = prio;
        }
    }
    s->exception_prio = active_prio;
    s->vectpending = pend_irq;
    s->vectpending_prio = pend_prio;
    nvic_irq_update(s);
}

NVICState *armv7m_nvic_new(ARMCPU *cpu)
{
    NVICState *s = g_new0(NVICState, 1);
    int i;

    s->cpu = cpu;
    s->num_vectors = NVIC_FIRST_IRQ + 64;
    s->prio_bits = 3;
    for (i = 1; i < NVIC_FIRST_IRQ; i++) {
        /* Fault handlers are enabled through SHCSR; the rest always are. */
        s->vec[i].enabled = !(i == ARMV7M_EXCP_MEM || i == ARMV7M_EXCP_BUS ||
                              i == ARMV7M_EXCP_USAGE);
    }
    nvic_recompute_state(s);
    return s;
}

void armv7m_nvic_free(NVICState *s)
{
    g_free(s);
}

void armv7m_nvic_reset(NVICState *s)
{
    int n = s->num_vectors;
    int bits = s->prio_bits;
    ARMCPU *cpu = s->cpu;

    memset(s, 0, sizeof(*s));
    s->cpu = cpu;
    s->num_vectors = n;
    s->prio_bits = bits;
    for (int i = 1; i < NVIC_FIRST_IRQ; i++) {
        s->vec[i].enabled = !(i == ARMV7M_EXCP_MEM || i == ARMV7M_EXCP_BUS ||
                              i == ARMV7M_EXCP_USAGE);
    }
    nvic_recompute_state(s);
}

void armv7m_nvic_config(NVICState *s, int num_irq, int prio_bits)
{
    if (num_irq < 1 || num_irq > NVIC_MAX_VECTORS - NVIC_FIRST_IRQ) {
        num_irq = NVIC_MAX_VECTORS - NVIC_FIRST_IRQ;
    }
    s->num_vectors = NVIC_FIRST_IRQ + num_irq;
    s->prio_bits = prio_bits < 1 ? 1 : (prio_bits > 8 ? 8 : prio_bits);
    nvic_recompute_state(s);
}

/* ---- the interface m_helper.c uses ---------------------------------- */

bool armv7m_nvic_can_take_pending_exception(void *opaque)
{
    NVICState *s = opaque;

    return s->vectpending && s->vectpending_prio < nvic_exec_prio(s);
}

int armv7m_nvic_raw_execution_priority(void *opaque)
{
    return ((NVICState *)opaque)->exception_prio;
}

bool armv7m_nvic_neg_prio_requested(void *opaque, bool secure)
{
    NVICState *s = opaque;

    return nvic_exec_prio(s) < 0;
}

bool armv7m_nvic_get_ready_status(void *opaque, int irq, bool secure)
{
    NVICState *s = opaque;

    return s->vec[irq].enabled &&
           nvic_group_prio(s, nvic_raw_prio(s, irq)) < nvic_exec_prio(s);
}

static void nvic_set_pending(NVICState *s, int irq, bool derived)
{
    CPUARMState *env = nvic_env(s);
    NvicVector *v;

    assert(irq > 0 && irq < s->num_vectors);

    /*
     * A synchronous fault (or SVC) that cannot preempt what is running, or
     * whose handler is disabled, escalates to HardFault.
     */
    if (irq == ARMV7M_EXCP_MEM || irq == ARMV7M_EXCP_BUS ||
        irq == ARMV7M_EXCP_USAGE || irq == ARMV7M_EXCP_SVC) {
        v = &s->vec[irq];
        if (!v->enabled ||
            nvic_group_prio(s, nvic_raw_prio(s, irq)) >= nvic_exec_prio(s) ||
            derived) {
            env->v7m.hfsr |= R_V7M_HFSR_FORCED_MASK;
            irq = ARMV7M_EXCP_HARD;
        }
    }
    if (irq == ARMV7M_EXCP_HARD && nvic_exec_prio(s) < -1) {
        /* Lockup: a HardFault cannot be taken. Leave it pending. */
    }
    s->vec[irq].pending = true;
    nvic_recompute_state(s);
}

void armv7m_nvic_set_pending(void *opaque, int irq, bool secure)
{
    nvic_set_pending(opaque, irq, false);
}

void armv7m_nvic_set_pending_derived(void *opaque, int irq, bool secure)
{
    nvic_set_pending(opaque, irq, true);
}

void armv7m_nvic_set_pending_lazyfp(void *opaque, int irq, bool secure)
{
    nvic_set_pending(opaque, irq, false);
}

void armv7m_nvic_get_pending_irq_info(void *opaque, int *pirq,
                                      bool *ptargets_secure)
{
    NVICState *s = opaque;

    *pirq = s->vectpending;
    *ptargets_secure = false;
}

void armv7m_nvic_acknowledge_irq(void *opaque)
{
    NVICState *s = opaque;
    CPUARMState *env = nvic_env(s);
    int irq = s->vectpending;
    NvicVector *v = &s->vec[irq];

    v->active = true;
    /* A level-sensitive line that is still high stays pending. */
    if (irq < NVIC_FIRST_IRQ || !v->level) {
        v->pending = false;
    }
    env->v7m.exception = irq;
    nvic_recompute_state(s);
}

int armv7m_nvic_complete_irq(void *opaque, int irq, bool secure)
{
    NVICState *s = opaque;
    NvicVector *v;
    int i;
    bool others_active = false;

    if (irq <= 0 || irq >= s->num_vectors || !s->vec[irq].active) {
        return -1;
    }
    v = &s->vec[irq];
    v->active = false;
    if (irq >= NVIC_FIRST_IRQ && v->level) {
        v->pending = true;
    }
    for (i = 1; i < s->num_vectors; i++) {
        if (s->vec[i].active) {
            others_active = true;
            break;
        }
    }
    nvic_recompute_state(s);
    return others_active ? 0 : 1;
}

/* ---- host-driven interrupt lines ------------------------------------ */

void armv7m_nvic_set_irq(NVICState *s, int exception, bool level)
{
    NvicVector *v;

    if (exception <= 0 || exception >= s->num_vectors) {
        return;
    }
    v = &s->vec[exception];
    if (level && !v->level) {
        v->pending = true;
    }
    v->level = level;
    nvic_recompute_state(s);
}

/* ---- System Control Space registers --------------------------------- */

static uint32_t scs_shcsr_read(NVICState *s)
{
    uint32_t r = s->shcsr_pend;

    r |= (s->vec[ARMV7M_EXCP_MEM].enabled ? 1u << 16 : 0);
    r |= (s->vec[ARMV7M_EXCP_BUS].enabled ? 1u << 17 : 0);
    r |= (s->vec[ARMV7M_EXCP_USAGE].enabled ? 1u << 18 : 0);
    r |= (s->vec[ARMV7M_EXCP_MEM].active ? 1u << 0 : 0);
    r |= (s->vec[ARMV7M_EXCP_BUS].active ? 1u << 1 : 0);
    r |= (s->vec[ARMV7M_EXCP_USAGE].active ? 1u << 3 : 0);
    r |= (s->vec[ARMV7M_EXCP_SVC].active ? 1u << 7 : 0);
    r |= (s->vec[ARMV7M_EXCP_DEBUG].active ? 1u << 8 : 0);
    r |= (s->vec[ARMV7M_EXCP_PENDSV].active ? 1u << 10 : 0);
    r |= (s->vec[ARMV7M_EXCP_SYSTICK].active ? 1u << 11 : 0);
    r |= (s->vec[ARMV7M_EXCP_MEM].pending ? 1u << 13 : 0);
    r |= (s->vec[ARMV7M_EXCP_BUS].pending ? 1u << 14 : 0);
    r |= (s->vec[ARMV7M_EXCP_USAGE].pending ? 1u << 12 : 0);
    r |= (s->vec[ARMV7M_EXCP_SVC].pending ? 1u << 15 : 0);
    return r;
}

static void scs_shcsr_write(NVICState *s, uint32_t value)
{
    s->vec[ARMV7M_EXCP_MEM].enabled = value & (1u << 16);
    s->vec[ARMV7M_EXCP_BUS].enabled = value & (1u << 17);
    s->vec[ARMV7M_EXCP_USAGE].enabled = value & (1u << 18);
    s->vec[ARMV7M_EXCP_SVC].pending = value & (1u << 15);
    nvic_recompute_state(s);
}

static uint32_t scs_icsr_read(NVICState *s)
{
    CPUARMState *env = nvic_env(s);
    uint32_t r = env->v7m.exception & 0x1ff;
    int i;
    bool others = false;

    /* RETTOBASE: no active exception other than the running one. */
    for (i = 1; i < s->num_vectors; i++) {
        if (s->vec[i].active && i != env->v7m.exception) {
            others = true;
        }
    }
    if (env->v7m.exception && !others) {
        r |= 1u << 11;
    }
    r |= (uint32_t)(s->vectpending & 0x1ff) << 12;
    if (s->vectpending >= NVIC_FIRST_IRQ) {
        r |= 1u << 22; /* ISRPENDING */
    }
    if (s->vec[ARMV7M_EXCP_SYSTICK].pending) {
        r |= 1u << 26;
    }
    if (s->vec[ARMV7M_EXCP_PENDSV].pending) {
        r |= 1u << 28;
    }
    if (s->vec[ARMV7M_EXCP_NMI].pending) {
        r |= 1u << 31;
    }
    return r;
}

static void scs_icsr_write(NVICState *s, uint32_t value)
{
    if (value & (1u << 31)) {
        s->vec[ARMV7M_EXCP_NMI].pending = true;
    }
    if (value & (1u << 28)) {
        s->vec[ARMV7M_EXCP_PENDSV].pending = true;
    }
    if (value & (1u << 27)) {
        s->vec[ARMV7M_EXCP_PENDSV].pending = false;
    }
    if (value & (1u << 26)) {
        s->vec[ARMV7M_EXCP_SYSTICK].pending = true;
    }
    if (value & (1u << 25)) {
        s->vec[ARMV7M_EXCP_SYSTICK].pending = false;
    }
    nvic_recompute_state(s);
}

static uint32_t scs_read32(NVICState *s, uint32_t offset)
{
    CPUARMState *env = nvic_env(s);
    int i, irq;
    uint32_t r;

    switch (offset) {
    case 0x004: /* ICTR */
        return (s->num_vectors - NVIC_FIRST_IRQ + 31) / 32 - 1;
    case 0x008: /* ACTLR */
        return 0;
    case 0xd00: /* CPUID: ARM Cortex-M4 r0p1 */
        return 0x410fc241;
    case 0xd04:
        return scs_icsr_read(s);
    case 0xd08:
        return env->v7m.vecbase[M_REG_NS];
    case 0xd0c:
        return (0xfa05u << 16) |
               (env->v7m.aircr & (R_V7M_AIRCR_PRIGROUP_MASK));
    case 0xd10:
        return env->v7m.scr[M_REG_NS];
    case 0xd14:
        return env->v7m.ccr[M_REG_NS];
    case 0xd18: case 0xd1c: case 0xd20:
        r = 0;
        for (i = 0; i < 4; i++) {
            irq = 4 + (offset - 0xd18) + i;
            r |= (uint32_t)s->vec[irq].prio << (8 * i);
        }
        return r;
    case 0xd24:
        return scs_shcsr_read(s);
    case 0xd28:
        return env->v7m.cfsr[M_REG_NS];
    case 0xd2c:
        return env->v7m.hfsr;
    case 0xd30:
        return env->v7m.dfsr;
    case 0xd34:
        return env->v7m.mmfar[M_REG_NS];
    case 0xd38:
        return env->v7m.bfar;
    case 0xd88: /* CPACR */
        return env->v7m.cpacr[M_REG_NS];
    case 0xf34: /* FPCCR */
        return env->v7m.fpccr[M_REG_S];
    case 0xf38: /* FPCAR */
        return env->v7m.fpcar[M_REG_S];
    case 0xf3c: /* FPDSCR */
        return env->v7m.fpdscr[M_REG_NS];
    default:
        break;
    }

    if (offset >= 0x100 && offset < 0x300) {
        /* ISER/ICER (enabled) and ISPR/ICPR (pending), 32 lines a word */
        int word = (offset & 0x7f) / 4;

        r = 0;
        for (i = 0; i < 32; i++) {
            irq = NVIC_FIRST_IRQ + word * 32 + i;
            if (irq >= s->num_vectors) {
                break;
            }
            if (offset < 0x200 ? s->vec[irq].enabled : s->vec[irq].pending) {
                r |= 1u << i;
            }
        }
        return r;
    }
    if (offset >= 0x300 && offset < 0x380) { /* IABR */
        int word = (offset & 0x7f) / 4;

        r = 0;
        for (i = 0; i < 32; i++) {
            irq = NVIC_FIRST_IRQ + word * 32 + i;
            if (irq < s->num_vectors && s->vec[irq].active) {
                r |= 1u << i;
            }
        }
        return r;
    }
    if (offset >= 0x400 && offset < 0x500) { /* IPR */
        r = 0;
        for (i = 0; i < 4; i++) {
            irq = NVIC_FIRST_IRQ + (offset - 0x400) + i;
            if (irq < s->num_vectors) {
                r |= (uint32_t)s->vec[irq].prio << (8 * i);
            }
        }
        return r;
    }
    return 0;
}

static void scs_write32(NVICState *s, uint32_t offset, uint32_t value)
{
    CPUARMState *env = nvic_env(s);
    int i, irq;
    const uint8_t prio_mask = (uint8_t)(0xff << (8 - s->prio_bits));

    switch (offset) {
    case 0xd04:
        scs_icsr_write(s, value);
        return;
    case 0xd08:
        env->v7m.vecbase[M_REG_NS] = value & 0xffffff80;
        env->v7m.vecbase[M_REG_S] = value & 0xffffff80;
        return;
    case 0xd0c:
        if ((value >> 16) != 0x05fa) {
            return;
        }
        env->v7m.aircr = (env->v7m.aircr & ~R_V7M_AIRCR_PRIGROUP_MASK) |
                         (value & R_V7M_AIRCR_PRIGROUP_MASK);
        if (value & R_V7M_AIRCR_SYSRESETREQ_MASK) {
            s->reset_requested = true;
            cpu_exit(CPU(s->cpu));
        }
        nvic_recompute_state(s);
        return;
    case 0xd10:
        env->v7m.scr[M_REG_NS] = value;
        return;
    case 0xd14:
        env->v7m.ccr[M_REG_NS] = value & 0x31b; /* NONBASETHRDENA, USERSETMPEND, UNALIGN_TRP, DIV_0_TRP, BFHFNMIGN, STKALIGN */
        return;
    case 0xd18: case 0xd1c: case 0xd20:
        for (i = 0; i < 4; i++) {
            irq = 4 + (offset - 0xd18) + i;
            s->vec[irq].prio = (value >> (8 * i)) & prio_mask;
        }
        nvic_recompute_state(s);
        return;
    case 0xd24:
        scs_shcsr_write(s, value);
        return;
    case 0xd28:
        env->v7m.cfsr[M_REG_NS] &= ~value; /* write one to clear */
        return;
    case 0xd2c:
        env->v7m.hfsr &= ~value;
        return;
    case 0xd30:
        env->v7m.dfsr &= ~value;
        return;
    case 0xd34:
        env->v7m.mmfar[M_REG_NS] = value;
        return;
    case 0xd38:
        env->v7m.bfar = value;
        return;
    case 0xd88: /* CPACR: CP10 and CP11 */
        env->v7m.cpacr[M_REG_NS] = value & (0xfu << 20);
        env->v7m.cpacr[M_REG_S] = env->v7m.cpacr[M_REG_NS];
        arm_rebuild_hflags(env);
        return;
    case 0xf34: /* FPCCR: ASPEN, LSPEN and the read-only status bits */
        env->v7m.fpccr[M_REG_S] = (value & 0xc0000000) |
                                  (env->v7m.fpccr[M_REG_S] & 0x3fffffff);
        env->v7m.fpccr[M_REG_NS] = env->v7m.fpccr[M_REG_S];
        arm_rebuild_hflags(env);
        return;
    case 0xf38:
        env->v7m.fpcar[M_REG_S] = value & ~7u;
        return;
    case 0xf3c:
        env->v7m.fpdscr[M_REG_NS] = value & 0x07c00000;
        env->v7m.fpdscr[M_REG_S] = env->v7m.fpdscr[M_REG_NS];
        return;
    case 0xf00: /* STIR */
        irq = NVIC_FIRST_IRQ + (value & 0x1ff);
        if (irq < s->num_vectors) {
            s->vec[irq].pending = true;
            nvic_recompute_state(s);
        }
        return;
    default:
        break;
    }

    if (offset >= 0x100 && offset < 0x300) {
        int word = (offset & 0x7f) / 4;
        int kind = (offset - 0x100) / 0x80; /* 0 ISER 1 ICER 2 ISPR 3 ICPR */

        for (i = 0; i < 32; i++) {
            if (!(value & (1u << i))) {
                continue;
            }
            irq = NVIC_FIRST_IRQ + word * 32 + i;
            if (irq >= s->num_vectors) {
                break;
            }
            switch (kind) {
            case 0: s->vec[irq].enabled = true; break;
            case 1: s->vec[irq].enabled = false; break;
            case 2: s->vec[irq].pending = true; break;
            case 3: s->vec[irq].pending = false; break;
            }
        }
        nvic_recompute_state(s);
        return;
    }
    if (offset >= 0x400 && offset < 0x500) {
        for (i = 0; i < 4; i++) {
            irq = NVIC_FIRST_IRQ + (offset - 0x400) + i;
            if (irq < s->num_vectors) {
                s->vec[irq].prio = (value >> (8 * i)) & prio_mask;
            }
        }
        nvic_recompute_state(s);
    }
}

/* Registers are 32 bits; narrower accesses (IPR and SHPR bytes) are merged. */
uint32_t armv7m_scs_read(NVICState *s, uint32_t offset)
{
    return scs_read32(s, offset & ~3u);
}

void armv7m_scs_write(NVICState *s, uint32_t offset, uint32_t value,
                      unsigned size)
{
    uint32_t base = offset & ~3u;

    if (size < 4) {
        unsigned shift = (offset & 3u) * 8;
        uint32_t mask = (size == 1 ? 0xffu : 0xffffu) << shift;
        bool bitwise = (base >= 0x100 && base < 0x300) || base == 0xd28 ||
                       base == 0xd2c || base == 0xd30;

        /*
         * Priority bytes merge into the register. Set/clear and write-one-to
         * -clear registers must not: merging would write back the other bits.
         */
        if (bitwise) {
            value = (value << shift) & mask;
        } else {
            value = (scs_read32(s, base) & ~mask) | ((value << shift) & mask);
        }
    }
    scs_write32(s, base, value);
}

bool armv7m_nvic_take_reset_request(NVICState *s)
{
    bool r = s->reset_requested;

    s->reset_requested = false;
    return r;
}

bool armv7m_nvic_has_pending(NVICState *s)
{
    return s->vectpending != 0;
}
