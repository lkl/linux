/*
 * Rump system call proxy interface for Linux
 * Copyright (c) 2015 Hajime Tazaki
 *
 * Author: Hajime Tazaki <thehajime@gmail.com>
 */

#include <linux/stddef.h>
#include <linux/types.h>
#include <generated/utsrelease.h>

#ifdef ENABLE_SYSPROXY
#include "rump.h"

extern struct rump_sysproxy_ops rump_sysproxy_ops;
int rump_init_server(const char *url)
{
	return rumpuser_sp_init(url, "Linux", UTS_RELEASE, "libos");
}

void rump_sysproxy_init(void)
{
	rump_init_server("unix:///tmp/rump-server");
}

void rump_sysproxy_fini(void)
{
	rumpuser_sp_fini(NULL);
}
#endif
