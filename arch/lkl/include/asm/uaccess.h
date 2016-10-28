#ifndef _ASM_LKL_UACCESS_H
#define _ASM_LKL_UACCESS_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/irqflags.h>
#include <linux/string.h>
#include <asm/errno.h>
#include <asm/thread_info.h>

#ifdef ENABLE_SYSPROXY
#include <rump/rumpuser.h>
#endif

#define __access_ok(addr, size) (1)

/* handle rump remote client */
static inline __must_check long __copy_from_user(void *to,
		const void __user *from, unsigned long n)
{
	int error = 0;
	struct thread_info *ti;

	ti = current_thread_info();

	if (unlikely(from == NULL && n))
		return -EFAULT;

	if (!ti->rump_client) {
		memcpy(to, from, n);
	} else if (n) {
#ifdef ENABLE_SYSPROXY
		error = rumpuser_sp_copyin(ti->rump_client, from, to, n);
#else
		;
#endif
	}

	return error;
}
#define __copy_from_user(to, from, n) __copy_from_user(to, from, n)

static inline __must_check long __copy_to_user(void __user *to,
		const void *from, unsigned long n)
{
	int error = 0;
	struct thread_info *ti;

	ti = current_thread_info();

	if (unlikely(to == NULL && n))
		return -EFAULT;

	if (!ti->rump_client) {
		memcpy(to, from, n);
	} else if (n) {
#ifdef ENABLE_SYSPROXY
		error = rumpuser_sp_copyout(ti->rump_client, from, to, n);
#else
		;
#endif
	}

	return error;
}
#define __copy_to_user(to, from, n) __copy_to_user(to, from, n)

#include <asm-generic/uaccess.h>

#endif /* _ASM_LKL_UACCESS_H */
