// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#include <asm/host_ops.h>
#include <asm/irq.h>

/*
 * KUnit coverage for the host-pthread caller path of lkl_trigger_irq.
 *
 * lkl_trigger_irq is documented as callable "from arbitrary host
 * threads" — backends like a libusb event thread post URB completions
 * by injecting an IRQ into the LKL kernel from outside any kernel
 * context. A host pthread doesn't own current_thread_info() (it never
 * went through __switch_to), so any per-thread state read from there
 * belongs to whichever kernel task most recently switched in.
 *
 * This test spawns a real host pthread via lkl_ops->thread_create,
 * has it call lkl_trigger_irq on a kernel-registered IRQ from outside
 * any kernel context, and asserts the handler runs synchronously —
 * the contract that host-thread-driven backends rely on.
 */

static struct completion handler_fired;
static atomic_t handler_runs;
static int test_irq;

static irqreturn_t test_irq_handler(int irq, void *data)
{
	atomic_inc(&handler_runs);
	complete(&handler_fired);
	return IRQ_HANDLED;
}

static void host_thread_trigger(void *arg)
{
	lkl_trigger_irq(*(int *)arg);
}

static void host_thread_irq_delivery_test(struct kunit *test)
{
	lkl_thread_t tid;
	long ret;

	atomic_set(&handler_runs, 0);
	init_completion(&handler_fired);

	test_irq = lkl_get_free_irq("lkl_irq_kunit");
	KUNIT_ASSERT_GE(test, test_irq, 0);

	ret = request_irq(test_irq, test_irq_handler, 0,
			  "lkl_irq_kunit", &test_irq);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * Spawn a true host pthread — outside any kernel context. It
	 * calls lkl_trigger_irq on the IRQ we just registered, exactly
	 * the path a libusb event thread (or any host-side backend
	 * notification thread) would take to inject a URB completion.
	 *
	 * wait_for_completion_timeout releases the LKL CPU so the host
	 * pthread can acquire it via lkl_cpu_try_run_irq inside
	 * lkl_trigger_irq.
	 */
	tid = lkl_ops->thread_create(host_thread_trigger, &test_irq);
	KUNIT_ASSERT_NE(test, (unsigned long)tid, 0UL);

	ret = wait_for_completion_timeout(&handler_fired,
					  msecs_to_jiffies(500));
	KUNIT_EXPECT_GT(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&handler_runs), 1);

	lkl_ops->thread_join(tid);
	free_irq(test_irq, &test_irq);
	lkl_put_irq(test_irq, "lkl_irq_kunit");
}

static struct kunit_case lkl_irq_kunit_test_cases[] = {
	KUNIT_CASE(host_thread_irq_delivery_test),
	{}
};

static struct kunit_suite lkl_irq_kunit_test_suite = {
	.name = "lkl_irq",
	.test_cases = lkl_irq_kunit_test_cases,
};

kunit_test_suite(lkl_irq_kunit_test_suite);

MODULE_LICENSE("GPL");
