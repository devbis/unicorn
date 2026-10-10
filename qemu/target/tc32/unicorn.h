#ifndef UC_QEMU_TARGET_TC32_H
#define UC_QEMU_TARGET_TC32_H

uc_err reg_read_tc32(void *env, int mode, unsigned int regid, void *value,
                     size_t *size);
uc_err reg_write_tc32(void *env, int mode, unsigned int regid,
                      const void *value, size_t *size, int *setpc);
void uc_init_tc32(struct uc_struct *uc);

#endif
