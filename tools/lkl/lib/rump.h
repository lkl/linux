/*
 * Rump hypercall interface for Linux
 * Copyright (c) 2015 Hajime Tazaki
 *
 * Author: Hajime Tazaki <thehajime@gmail.com>
 */

#define __dead
#define __printflike(x, y)
#include <rump/rumpuser.h>

struct irq_data;

void rump_sysproxy_init(void);
void rump_sysproxy_fini(void);

extern const struct rumpuser_hyperup hyp;
#ifdef ENABLE_SYSPROXY
extern struct rump_sysproxy_ops rump_sysproxy_ops;
#endif

int rump_pci_irq_request(struct irq_data *data);
void rump_pci_irq_release(struct irq_data *data);
