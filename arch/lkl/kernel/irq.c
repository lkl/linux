#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/hardirq.h>
#include <asm/irq_regs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/tick.h>
#include <asm/irqflags.h>
#include <asm/host_ops.h>
#include <asm/cpu.h>

static unsigned long irq_status;
static bool irqs_enabled;

#define TEST_AND_CLEAR_IRQ_STATUS(x)	__sync_fetch_and_and(&irq_status, 0)
#define IRQ_BIT(x)			BIT(x-1)
#define SET_IRQ_STATUS(x)		__sync_fetch_and_or(&irq_status, BIT(x - 1))

static struct irq_info {
	const char *user;
} irqs[NR_IRQS];

/**
 * This function can be called from arbitrary host threads, so do not
 * issue any Linux calls (e.g. prink) if lkl_cpu_get() was not issued
 * before.
 */
int lkl_trigger_irq(int irq)
{
	if (!irq || irq > NR_IRQS || lkl_cpu_is_shutdown())
		return -EINVAL;

	if (lkl_cpu_try_get()) {
		bool resched;
		unsigned long flags;

		/* since this can be called from Linux context
		 * (e.g. lkl_trigger_irq -> IRQ -> softirq ->
		 * lkl_trigger_irq) make sure we are actually allowed
		 * to run irqs at this point */
		if (!irqs_enabled) {
			lkl_cpu_put();
			goto delayed_irq;
		}

		/* interrupts handlers need to run with interrupts disabled */
		local_irq_save(flags);
		irq_enter();
		generic_handle_irq(irq);
		irq_exit();
		local_irq_restore(flags);

		resched = need_resched();

		lkl_cpu_put();

		if (resched)
			lkl_cpu_wakeup();

		return 0;
	}

delayed_irq:
	SET_IRQ_STATUS(irq);
	lkl_cpu_wakeup();

	return 0;
}

static void run_irqs(void)
{
	int i = 1;
	unsigned long status;

	if (!irq_status)
		return;

	status = TEST_AND_CLEAR_IRQ_STATUS(IRQS_MASK);

	while (status) {
		if (status & 1) {
			irq_enter();
			generic_handle_irq(i);
			irq_exit();
		}
		status = status >> 1;
		i++;
	}
}

int show_interrupts(struct seq_file *p, void *v)
{
	return 0;
}

int lkl_get_free_irq(const char *user)
{
	int i;
	int ret = -EBUSY;

	/* 0 is not a valid IRQ */
	for (i = 1; i < NR_IRQS; i++) {
		if (!irqs[i].user) {
			irqs[i].user = user;
			ret = i;
			break;
		}
	}

	return ret;
}

void lkl_put_irq(int i, const char *user)
{
	if (!irqs[i].user || strcmp(irqs[i].user, user) != 0) {
		WARN("%s tried to release %s's irq %d", user, irqs[i].user, i);
		return;
	}

	irqs[i].user = NULL;
}

unsigned long arch_local_save_flags(void)
{
	return irqs_enabled;
}

void arch_local_irq_restore(unsigned long flags)
{
	if (flags == ARCH_IRQ_ENABLED && irqs_enabled == ARCH_IRQ_DISABLED &&
	    !in_interrupt())
		run_irqs();
	irqs_enabled = flags;
}

void init_IRQ(void)
{
	int i;

	for (i = 0; i < NR_IRQS; i++)
		irq_set_chip_and_handler(i, &dummy_irq_chip, handle_simple_irq);

	pr_info("lkl: irqs initialized\n");
}

void cpu_yield_to_irqs(void)
{
	cpu_relax();
}
