/*
 * Rump hypercall interface for Linux
 * Copyright (c) 2015 Hajime Tazaki
 *
 * Author: Hajime Tazaki <thehajime@gmail.com>
 */

#define __dead
#define __printflike(x, y)
#include <rump/rumpuser.h>

#ifdef RUMPRUN
#define sscanf rumpns_sscanf
extern int sscanf(const char *, const char *, ...);

#define vsnprintf rumpns_vsnprintf
typedef __builtin_va_list va_list;
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#define strncat rumpns_strncat
extern char * strncat(char *, const char *, size_t);

#define strlen rumpns_strlen
extern size_t strlen(const char *);

#define strncmp rumpns_strncmp
extern int strncmp(const char *,const char *, size_t);

#define strncpy rumpns_strncpy
extern char * strncpy(char *,const char *, size_t);

#define snprintf rumpns_snprintf
int snprintf(char *buf, size_t size, const char *fmt, ...);

#define memset rumpns_memset
extern void *memset(void *, int, size_t);

#define memcmp rumpns_memcmp
extern int memcmp(const void *,const void *, size_t);

#define memcpy rumpns_memcpy
extern void * memcpy(void *,const void *, size_t);

#endif

struct irq_data;

void rump_sysproxy_init(void);
void rump_sysproxy_fini(void);

extern const struct rumpuser_hyperup hyp;

int rump_pci_irq_request(struct irq_data *data);
void rump_pci_irq_release(struct irq_data *data);
