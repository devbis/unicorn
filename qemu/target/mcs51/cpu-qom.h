#ifndef QEMU_MCS51_CPU_QOM_H
#define QEMU_MCS51_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_MCS51_CPU "mcs51-cpu"
#define MCS51_CPU(obj) ((MCS51CPU *)(obj))
#define MCS51_CPU_CLASS(klass) ((MCS51CPUClass *)(klass))
#define MCS51_CPU_GET_CLASS(obj) (&((MCS51CPU *)(obj))->cc)

typedef struct MCS51CPUClass {
    CPUClass parent_class;
    void (*parent_reset)(CPUState *cpu);
} MCS51CPUClass;

typedef struct MCS51CPU MCS51CPU;

#endif
