#ifndef QEMU_TC32_CPU_QOM_H
#define QEMU_TC32_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_TC32_CPU "tc32-cpu"
#define TC32_CPU(obj) ((TC32CPU *)(obj))
#define TC32_CPU_CLASS(klass) ((TC32CPUClass *)(klass))
#define TC32_CPU_GET_CLASS(obj) (&((TC32CPU *)(obj))->cc)

typedef struct TC32CPUClass {
    CPUClass parent_class;
    void (*parent_reset)(CPUState *cpu);
} TC32CPUClass;

typedef struct TC32CPU TC32CPU;

#endif
