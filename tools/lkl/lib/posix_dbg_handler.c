#include <signal.h>
#include <stddef.h>
#include <stdio.h>

extern void dbg_handler(int);

void lkl_register_dbg_handler() {
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = dbg_handler;
	if (sigaction(SIGTSTP, &sa, NULL) == -1) {
		perror("sigaction");
	}
}

