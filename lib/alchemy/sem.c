/*
 * Copyright (C) 2011 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include <errno.h>
#include <string.h>
#include <copperplate/threadobj.h>
#include <copperplate/heapobj.h>
#include "reference.h"
#include "internal.h"
#include "sem.h"
#include "timer.h"

struct syncluster alchemy_sem_table;

static struct alchemy_namegen sem_namegen = {
	.prefix = "sem",
	.length = sizeof ((struct alchemy_sem *)0)->name,
};

DEFINE_LOOKUP_PRIVATE(sem, RT_SEM);

static void sem_finalize(struct semobj *smobj)
{
	struct alchemy_sem *scb = container_of(smobj, struct alchemy_sem, smobj);
	/* We should never fail here, so we backtrace. */
	__bt(syncluster_delobj(&alchemy_sem_table, &scb->cobj));
	scb->magic = ~sem_magic;
	xnfree(scb);
}
fnref_register(libalchemy, sem_finalize);

int rt_sem_create(RT_SEM *sem, const char *name,
		  unsigned long icount, int mode)
{
	int smobj_flags = 0, ret;
	struct alchemy_sem *scb;
	struct service svc;

	if (threadobj_irq_p())
		return -EPERM;

	if (mode & S_PULSE) {
		if (icount > 0)
			return -EINVAL;
		smobj_flags |= SEMOBJ_PULSE;
	}

	COPPERPLATE_PROTECT(svc);

	scb = xnmalloc(sizeof(*scb));
	if (scb == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (mode & S_PRIO)
		smobj_flags |= SEMOBJ_PRIO;

	ret = semobj_init(&scb->smobj, smobj_flags, icount,
			  fnref_put(libalchemy, sem_finalize));
	if (ret) {
		xnfree(scb);
		goto out;
	}

	alchemy_build_name(scb->name, name, &sem_namegen);
	scb->magic = sem_magic;

	if (syncluster_addobj(&alchemy_sem_table, scb->name, &scb->cobj)) {
		semobj_destroy(&scb->smobj);
		xnfree(scb);
		ret = -EEXIST;
	} else
		sem->handle = mainheap_ref(scb, uintptr_t);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_sem_delete(RT_SEM *sem)
{
	struct alchemy_sem *scb;
	struct service svc;
	int ret = 0;

	if (threadobj_irq_p())
		return -EPERM;

	COPPERPLATE_PROTECT(svc);

	scb = find_alchemy_sem(sem, &ret);
	if (scb == NULL)
		goto out;

	/*
	 * XXX: we rely on copperplate's semobj to check for semaphore
	 * existence, so we refrain from altering the object memory
	 * until we know it was valid. So the only safe place to
	 * negate the magic tag, deregister from the cluster and
	 * release the memory is in the finalizer routine, which is
	 * only called for valid objects.
	 */
	ret = semobj_destroy(&scb->smobj);
	if (ret > 0)
		ret = 0;
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_sem_p_timed(RT_SEM *sem, const struct timespec *abs_timeout)
{
	struct alchemy_sem *scb;
	struct service svc;
	int ret = 0;

	COPPERPLATE_PROTECT(svc);

	scb = find_alchemy_sem(sem, &ret);
	if (scb == NULL)
		goto out;

	ret = semobj_wait(&scb->smobj, abs_timeout);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_sem_v(RT_SEM *sem)
{
	struct alchemy_sem *scb;
	struct service svc;
	int ret = 0;

	COPPERPLATE_PROTECT(svc);

	scb = find_alchemy_sem(sem, &ret);
	if (scb == NULL)
		goto out;

	ret = semobj_post(&scb->smobj);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_sem_broadcast(RT_SEM *sem)
{
	struct alchemy_sem *scb;
	struct service svc;
	int ret = 0;

	COPPERPLATE_PROTECT(svc);

	scb = find_alchemy_sem(sem, &ret);
	if (scb == NULL)
		goto out;

	ret = semobj_broadcast(&scb->smobj);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_sem_inquire(RT_SEM *sem, RT_SEM_INFO *info)
{
	struct alchemy_sem *scb;
	struct service svc;
	int ret = 0, sval;

	COPPERPLATE_PROTECT(svc);

	scb = find_alchemy_sem(sem, &ret);
	if (scb == NULL)
		goto out;

	ret = semobj_getvalue(&scb->smobj, &sval);
	if (ret)
		goto out;

	info->count = sval < 0 ? 0 : sval;
	info->nwaiters = -sval;
	strcpy(info->name, scb->name); /* <= racy. */
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_sem_bind(RT_SEM *sem,
		const char *name, RTIME timeout)
{
	return alchemy_bind_object(name,
				   &alchemy_sem_table,
				   timeout,
				   offsetof(struct alchemy_sem, cobj),
				   &sem->handle);
}

int rt_sem_unbind(RT_SEM *sem)
{
	sem->handle = 0;
	return 0;
}