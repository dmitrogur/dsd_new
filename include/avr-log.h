#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ippl_new(const char *type);

void ippl_add(const char *key, const char *value);
void ippl_adds(const char *key, const char *value);
void ippl_addi(const char *key, int value);
void ippl_addu(const char *key, unsigned value);
void ippl_addl(const char *key, unsigned long long value);

void ilog(const char *str);

void ipp_last_sample_num(void);

#ifdef __cplusplus
}
#endif
