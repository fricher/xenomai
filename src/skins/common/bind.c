#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#include <asm/xenomai/syscall.h>
#include <asm-generic/xenomai/bits/bind.h>
#include <asm-generic/xenomai/bits/current.h>
#include <asm-generic/xenomai/stacksize.h>
#include "sem_heap.h"

static pthread_t xeno_main_tid;
static xnsighandler *xnsig_handlers[32];

void __attribute__((weak)) xeno_sigill_handler(int sig)
{
	fprintf(stderr, "Xenomai or CONFIG_XENO_OPT_PERVASIVE disabled.\n"
		"(modprobe xeno_nucleus?)\n");
	exit(EXIT_FAILURE);
}

struct xnfeatinfo xeno_featinfo;

int __xnsig_dispatch(struct xnsig *sigs, int cumulated_error, int last_error)
{
	unsigned i;

  dispatch:
	for (i = 0; i < sigs->nsigs; i++) {
		xnsighandler *handler;
		
		handler = xnsig_handlers[sigs->pending[i].muxid];
		if (handler)
			handler(&sigs->pending[i].si);
	}

	if (cumulated_error == -ERESTART)
		cumulated_error = last_error;

	if (sigs->remaining) {
		sigs->nsigs = 0;
		last_error = XENOMAI_SYSSIGS(sigs);
		if (sigs->nsigs)
			goto dispatch;
	}

	return cumulated_error;
}

#ifdef XENOMAI_SYSSIGS_SAFE
int __xnsig_dispatch_safe(struct xnsig *sigs, int cumulated_error, int last_error)
{
	unsigned i;

  dispatch:
	for (i = 0; i < sigs->nsigs; i++) {
		xnsighandler *handler;
		
		handler = xnsig_handlers[sigs->pending[i].muxid];
		if (handler)
			handler(&sigs->pending[i].si);
	}

	if (cumulated_error == -ERESTART)
		cumulated_error = last_error;

	if (sigs->remaining) {
		sigs->nsigs = 0;
		last_error = XENOMAI_SYSSIGS_SAFE(sigs);
		if (sigs->nsigs)
			goto dispatch;
	}

	return cumulated_error;
}
#endif /* XENOMAI_SYSSIGS_SAFE */

int 
xeno_bind_skin_opt(unsigned skin_magic, const char *skin, 
		   const char *module, xnsighandler *handler)
{
	sighandler_t old_sigill_handler;
	xnfeatinfo_t finfo;
	int muxid;

	old_sigill_handler = signal(SIGILL, xeno_sigill_handler);
	if (old_sigill_handler == SIG_ERR) {
		perror("signal(SIGILL)");
		exit(EXIT_FAILURE);
	}

	muxid = XENOMAI_SYSBIND(skin_magic,
				XENOMAI_FEAT_DEP, XENOMAI_ABI_REV, &finfo);

	signal(SIGILL, old_sigill_handler);

	switch (muxid) {
	case -EINVAL:

		fprintf(stderr, "Xenomai: incompatible feature set\n");
		fprintf(stderr,
			"(userland requires \"%s\", kernel provides \"%s\", missing=\"%s\").\n",
			finfo.feat_man_s, finfo.feat_all_s, finfo.feat_mis_s);
		exit(EXIT_FAILURE);

	case -ENOEXEC:

		fprintf(stderr, "Xenomai: incompatible ABI revision level\n");
		fprintf(stderr, "(user-space requires '%lu', kernel provides '%lu').\n",
			XENOMAI_ABI_REV, finfo.feat_abirev);
		exit(EXIT_FAILURE);

	case -ENOSYS:
	case -ESRCH:

		return -1;
	}

	if (muxid < 0) {
		fprintf(stderr, "Xenomai: binding failed: %s.\n",
			strerror(-muxid));
		exit(EXIT_FAILURE);
	}

	xnsig_handlers[muxid] = handler;

#ifdef xeno_arch_features_check
	xeno_arch_features_check();
#endif /* xeno_arch_features_check */

	xeno_init_sem_heaps();

	xeno_init_current_keys();

	xeno_featinfo = finfo;

	xeno_main_tid = pthread_self();
	
	return muxid;
}

void xeno_fault_stack(void)
{
	char stk[xeno_stacksize(1)];

	if (pthread_self() != xeno_main_tid)
		return;

	stk[0] = stk[sizeof(stk) - 1] = 0xA5;
}
