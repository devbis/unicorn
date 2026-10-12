#ifndef UNICORN_MCS51_TARGET_H
#define UNICORN_MCS51_TARGET_H

#include "unicorn/unicorn.h"

uc_err reg_read_mcs51(void *env, int mode, unsigned int regid, void *value,
                      size_t *size);
uc_err reg_write_mcs51(void *env, int mode, unsigned int regid,
                       const void *value, size_t *size, int *setpc);
void uc_init_mcs51(struct uc_struct *uc);

#endif
