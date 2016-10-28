#include <linux/sched.h>

static void handle_signal(struct ksignal *ksig, struct pt_regs *regs)
{
	ksig->ka.sa.sa_handler(ksig->sig);
}

void do_signal(struct pt_regs *regs)
{
	struct ksignal ksig;

	while (get_signal(&ksig)) {
		/* Whee!  Actually deliver the signal.  */
		handle_signal(&ksig, regs);
	}
}
