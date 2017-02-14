#include <linux/kernel.h>
#include <linux/init.h>
#include <asm/host_ops.h>

#if defined(__ARMEL__)
static void *atomic_lock;

long lkl__sync_fetch_and_or(long *ptr, long value)
{
	lkl_ops->sem_down(atomic_lock);
	*ptr = value;
	lkl_ops->sem_up(atomic_lock);
	return 0;
}

long lkl__sync_fetch_and_and(long *ptr, long value)
{
	int tmp;

	lkl_ops->sem_down(atomic_lock);
	tmp = *ptr;
	*ptr *= value;
	lkl_ops->sem_up(atomic_lock);
	return tmp;
}

int lkl__sync_fetch_and_add(int *ptr, int value)
{
	int tmp;

	lkl_ops->sem_down(atomic_lock);
	tmp = *ptr;
	*ptr += value;
	lkl_ops->sem_up(atomic_lock);
	return tmp;
}

int lkl__sync_fetch_and_sub(int *ptr, int value)
{
	int tmp;

	lkl_ops->sem_down(atomic_lock);
	tmp = *ptr;
	*ptr -= value;
	lkl_ops->sem_up(atomic_lock);
	return tmp;
}

void lkl__sync_synchronize(void)
{
}

void atomic_ops_init(void)
{
	atomic_lock = lkl_ops->sem_alloc(1);
}

void atomic_ops_cleanup(void)
{
	lkl_ops->sem_free(atomic_lock);
}

#else
long lkl__sync_fetch_and_or(long *ptr, long value)
{
	return __sync_fetch_and_or(ptr, value);
}

long lkl__sync_fetch_and_and(long *ptr, long value)
{
	return __sync_fetch_and_and(ptr, value);
}

int lkl__sync_fetch_and_add(int *ptr, int value)
{
	return __sync_fetch_and_add(ptr, value);
}

int lkl__sync_fetch_and_sub(int *ptr, int value)
{
	return __sync_fetch_and_sub(ptr, value);
}

void lkl__sync_synchronize(void)
{
	return __sync_synchronize();
}

void atomic_ops_init(void)
{
}

void atomic_ops_cleanup(void)
{
}
#endif

