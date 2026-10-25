#ifndef BOOT_VERIFY_SUPPORT_H
#define BOOT_VERIFY_SUPPORT_H

void *memcpy(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int c, unsigned int n);
int memcmp(const void *a, const void *b, unsigned int n);
void boot_verify_fail(void);
void boot_verify_ok(void);

#endif
