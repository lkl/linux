#ifndef _ASM_UAPI_LKL_HOST_OPS_H
#define _ASM_UAPI_LKL_HOST_OPS_H

/* Defined in {posix,nt}-host.c */
struct lkl_mutex_t;
struct lkl_sem_t;

/**
 * lkl_host_operations - host operations used by the Linux kernel
 *
 * These operations must be provided by a host library or by the application
 * itself.
 *
 * @virtio_devices - string containg the list of virtio devices in virtio mmio
 * command line format. This string is appended to the kernel command line and
 * is provided here for convenience to be implemented by the host library.
 *
 * @print - optional operation that receives console messages
 *
 * @panic - called during a kernel panic
 *
 * @sem_alloc - allocate a host semaphore an initialize it to count
 * @sem_free - free a host semaphore
 * @sem_up - perform an up operation on the semaphore
 * @sem_down - perform a down operation on the semaphore
 * @sem_get - return the current value of semahpore. To be used only for sanity checking
 *
 * @mutex_alloc - allocate and initialize a host mutex
 * @mutex_free - free a host mutex
 * @mutex_lock - acquire the mutex
 * @mutex_unlock - release the mutex
 *
 * @thread_create - create a new thread and run f(arg) in its context; returns a
 * thread handle or NULL if the thread could not be created
 * @thread_exit - terminates the current thread
 *
 * @tls_alloc - allocate a thread local storage key; returns 0 if succesful
 * @tls_free - frees a thread local storage key; returns 0 if succesful
 * @tls_set - associate data to the thread local storage key; returns 0 if succesful
 * @tls_get - return data associated with the thread local storage key or NULL on error
 *
 * @mem_alloc - allocate memory
 * @mem_free - free memory
 *
 * @timer_create - allocate a host timer that runs fn(arg) when the timer
 * fires.
 * @timer_free - disarms and free the timer
 * @timer_set_oneshot - arm the timer to fire once, after delta ns.
 * @timer_set_periodic - arm the timer to fire periodically, with a period of
 * delta ns.
 *
 * @ioremap - searches for an I/O memory region identified by addr and size and
 * returns a pointer to the start of the address range that can be used by
 * iomem_access
 * @iomem_acess - reads or writes to and I/O memory region; addr must be in the
 * range returned by ioremap
 *
 * @gettid - returns the host thread id of the caller
 */
struct lkl_host_operations {
	const char *virtio_devices;

	void (*print)(const char *str, int len);
	void (*panic)(void);

	struct lkl_sem_t* (*sem_alloc)(int count);
	void (*sem_free)(struct lkl_sem_t *sem);
	void (*sem_up)(struct lkl_sem_t *sem);
	void (*sem_down)(struct lkl_sem_t *sem);
	int (*sem_get)(struct lkl_sem_t *sem);

	struct lkl_mutex_t *(*mutex_alloc)(void);
	void (*mutex_free)(struct lkl_mutex_t *mutex);
	void (*mutex_lock)(struct lkl_mutex_t *mutex);
	void (*mutex_unlock)(struct lkl_mutex_t *mutex);

	int (*thread_create)(void (*f)(void *), void *arg);
	void (*thread_exit)(void);

	int (*tls_alloc)(unsigned int *key);
	int (*tls_free)(unsigned int key);
	int (*tls_set)(unsigned int key, void *data);
	void *(*tls_get)(unsigned int key);

	void* (*mem_alloc)(unsigned long);
	void (*mem_free)(void *);

	unsigned long long (*time)(void);

	void* (*timer_alloc)(void (*fn)(void *), void *arg);
	int (*timer_set_oneshot)(void *timer, unsigned long delta);
	void (*timer_free)(void *timer);

	void* (*ioremap)(long addr, int size);
	int (*iomem_access)(const volatile void *addr, void *val, int size,
			    int write);

	long (*gettid)(void);
};

/**
 * lkl_start_kernel - registers the host operations and starts the kernel
 *
 * The function returns only after the kernel is shutdown with lkl_sys_halt.
 *
 * @lkl_ops - pointer to host operations
 * @mem_size - how much memory to allocate to the Linux kernel
 * @cmd_line - format for command line string that is going to be used to
 * generate the Linux kernel command line
 */
int lkl_start_kernel(struct lkl_host_operations *lkl_ops,
		     unsigned long mem_size,
		     const char *cmd_line, ...);

#endif
