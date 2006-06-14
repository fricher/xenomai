/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@laposte.net>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/**
 * @ingroup posix
 * @defgroup posix_sem Semaphores services.
 *
 * Semaphores services.
 *
 * Semaphores are counters for resources shared between threads. The basic
 * operations on semaphores are: increment the counter atomically, and wait
 * until the counter is non-null and decrement it atomically.
 *
 * Semaphores have a maximum value past which they cannot be incremented.  The
 * macro @a SEM_VALUE_MAX is defined to be this maximum value.
 *
 *@{*/

#include <stddef.h>
#include <stdarg.h>

#include <posix/registry.h>     /* For named semaphores. */
#include <posix/thread.h>
#include <posix/sem.h>

typedef struct pse51_sem {
    unsigned magic;
    xnsynch_t synchbase;
    xnholder_t link;            /* Link in pse51_semq */

#define link2sem(laddr)                                                 \
    ((pse51_sem_t *)(((char *)(laddr)) - offsetof(pse51_sem_t, link)))

    int value;
} pse51_sem_t;

typedef struct pse51_named_sem {
    pse51_sem_t sembase;        /* Has to be the first member. */
#define sem2named_sem(saddr) ((nsem_t *) (saddr))
    
    pse51_node_t nodebase;
#define node2sem(naddr) \
    ((nsem_t *)((char *)(naddr) - offsetof(nsem_t, nodebase)))

#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)
    xnqueue_t userq;            /* List of user-space bindings. */
#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */    

    union __xeno_sem descriptor;
} nsem_t;

#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)
typedef struct pse51_uptr {
    struct mm_struct *mm;
    unsigned refcnt;
    unsigned long uaddr;

    xnholder_t link;

#define link2uptr(laddr) \
    ((pse51_uptr_t *)((char *)(laddr) - offsetof(pse51_uptr_t, link)))
} pse51_uptr_t;
#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */    

static xnqueue_t pse51_semq;

static void sem_destroy_internal (pse51_sem_t *sem)

{
    removeq(&pse51_semq, &sem->link);    
    if (xnsynch_destroy(&sem->synchbase) == XNSYNCH_RESCHED)
        xnpod_schedule();

    pse51_mark_deleted(sem);
    xnfree(sem);
}

/* Called with nklock locked, irq off. */
static int pse51_sem_init_inner (pse51_sem_t *sem, int pshared, unsigned value)
{
    if (value > (unsigned) SEM_VALUE_MAX)
        return EINVAL;
    
    sem->magic = PSE51_SEM_MAGIC;
    inith(&sem->link);
    appendq(&pse51_semq, &sem->link);    
    xnsynch_init(&sem->synchbase, XNSYNCH_PRIO);
    sem->value = value;

    return 0;
}


/**
 * Initialize an unnamed semaphore.
 *
 * This service initializes the semaphore @a sm, with the value @a value.
 *
 * The @a pshared argument is ignored by this implementation, semaphores created
 * by this service may be shared by kernel-space modules and user-space
 * processes through shared memory.
 *
 * This service fails if @a sm is already initialized or is a named semaphore.
 *
 * @param sm the semaphore to be initialized;
 *
 * @param pshared ignored;
 *
 * @param value the semaphore initial value.
 *
 * @retval 0 on success,
 * @retval -1 with @a errno set if:
 * - EBUSY, the semaphore @a sm was already initialized;
 * - ENOSPC, insufficient memory exists in the system heap to initialize the
 *   semaphore, increase CONFIG_XENO_OPT_SYS_HEAPSZ;
 * - EINVAL, the @a value argument exceeds @a SEM_VALUE_MAX.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_init.html">
 * Specification.</a>
 * 
 */
int sem_init (sem_t *sm, int pshared, unsigned value)
{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    pse51_sem_t *sem;
    int err;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (shadow->magic == PSE51_SEM_MAGIC
        || shadow->magic == PSE51_NAMED_SEM_MAGIC
        || shadow->magic == ~PSE51_NAMED_SEM_MAGIC)
        {
        xnholder_t *holder;
        
        for (holder = getheadq(&pse51_semq); holder;
             holder = nextq(&pse51_semq, holder))
            if (holder == &shadow->sem->link)
                {
                err = EBUSY;
                goto error;
                }
        }

    sem = (pse51_sem_t *) xnmalloc(sizeof(pse51_sem_t));
    if (!sem)
        {
        err = ENOSPC;
        goto error;
        }

    err = pse51_sem_init_inner(sem, pshared, value);
    if (err)
        {
        xnfree(sem);
        goto error;
        }

    shadow->magic = PSE51_SEM_MAGIC;
    shadow->sem = sem;
    xnlock_put_irqrestore(&nklock, s);

    return 0;

  error:
    xnlock_put_irqrestore(&nklock, s);
    thread_set_errno(err);

    return -1;
}

/**
 * Destroy an unnamed semaphore.
 *
 * This service destroys the semaphore @a sm. Threads currently blocked on @a sm
 * are unblocked and the service they called return -1 with @a errno set to
 * EINVAL. The semaphore is then considered invalid by all semaphore services
 * (they all fail with @a errno set to EINVAL) except sem_init().
 *
 * This service fails if @a sm is a named semaphore.
 *
 * @param sm the semaphore to be destroyed.
 *
 * @retval 0 on success,
 * @retval -1 with @a errno set if:
 * - EINVAL, the semaphore @a sm is invalid or a named semaphore.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_destroy.html">
 * Specification.</a>
 * 
 */
int sem_destroy (sem_t *sm)

{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (shadow->magic != PSE51_SEM_MAGIC)
        {
        thread_set_errno(EINVAL);
        goto error;
        }
    
    sem_destroy_internal(shadow->sem);
    pse51_mark_deleted(shadow);

    xnlock_put_irqrestore(&nklock, s);

    return 0;

  error:

    xnlock_put_irqrestore(&nklock, s);

    return -1;
}

/**
 * Open a named semaphore.
 *
 * This service establishes a connection between the semaphore named @a name and
 * the calling context (kernel-space as a whole, or user-space process).
 *
 * If no semaphore named @a name exists and @a oflags has the @a O_CREAT bit
 * set, the semaphore is created by this function, using two more arguments:
 * - a @a mode argument, of type @b mode_t, currently ignored;
 * - a @a value argument, of type @b unsigned, specifying the initial value of
 *   the created semaphore.
 *
 * If @a oflags has the two bits @a O_CREAT and @a O_EXCL set and the semaphore
 * already exists, this service fails.
 *
 * @a name may be any arbitrary string, in which slashes have no particular
 * meaning. However, for portability, using a name which starts with a slash and
 * contains no other slash is recommended.
 *
 * If sem_open() is called from the same context (kernel-space as a whole, or
 * user-space process) several times with the same value of @a name, the same
 * address is returned.
 *
 * @param name the name of the semaphore to be created;
 *
 * @param oflags flags.
 *
 * @return the address of the named semaphore on success;
 * @return SEM_FAILED with @a errno set if:
 * - ENAMETOOLONG, the length of the @a name argument exceeds 64 characters;
 * - EEXIST, the bits @a O_CREAT and @a O_EXCL were set in @a oflags and the
 *   named semaphore already exists;
 * - ENOENT, the bit @a O_CREAT is not set in @a oflags and the named semaphore
 *   does not exist;
 * - ENOSPC, insufficient memory exists in the system heap to create the
 *   semaphore, increase CONFIG_XENO_OPT_SYS_HEAPSZ;
 * - EINVAL, the @a value argument exceeds @a SEM_VALUE_MAX.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_open.html">
 * Specification.</a>
 * 
 */
sem_t *sem_open (const char *name, int oflags, ...)
{
    nsem_t *named_sem;
    pse51_node_t *node;
    spl_t s;
    int err;

    xnlock_get_irqsave(&nklock, s);

    err = pse51_node_get(&node, name, PSE51_NAMED_SEM_MAGIC, oflags);

    if (err)
        goto error;

    if (!node)
        {
        unsigned value;
        mode_t mode;
        va_list ap;

        named_sem = (nsem_t *) xnmalloc(sizeof(*named_sem));

        if (!named_sem)
            {
            err = ENOSPC;
            goto error;
            }
        
        va_start(ap, oflags);
        mode = va_arg(ap, int); /* unused */
        value = va_arg(ap, unsigned);
        va_end(ap);

        err = pse51_sem_init_inner(&named_sem->sembase, 1, value);

        if (err)
            {
            xnfree(named_sem);
            goto error;
            }

        err = pse51_node_add(&named_sem->nodebase, name, PSE51_NAMED_SEM_MAGIC);

        if (err)
            {
            xnfree(named_sem);
            goto error;
            }

#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)
        initq(&named_sem->userq);
#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */
        named_sem->descriptor.shadow_sem.sem = &named_sem->sembase;
        named_sem->sembase.magic = PSE51_NAMED_SEM_MAGIC;
        }
    else
        named_sem = node2sem(node);
    
    /* Set the magic, needed both at creation and when re-opening a semaphore
       that was closed but not unlinked. */
    named_sem->descriptor.shadow_sem.magic = PSE51_NAMED_SEM_MAGIC;

    xnlock_put_irqrestore(&nklock, s);

    return &named_sem->descriptor.native_sem;

  error:

    xnlock_put_irqrestore(&nklock, s);

    thread_set_errno(err);

    return SEM_FAILED;
}

/**
 * Close a named semaphore.
 *
 * This service closes the semaphore @a sm. The semaphore is destroyed only when
 * unlinked with a call to the sem_unlink() service and when each call to
 * sem_open() matches a call to this service.
 *
 * When a semaphore is destroyed, the memory it used is returned to the system
 * heap, so that further references to this semaphore are not guaranteed to
 * fail, as is the case for unnamed semaphores.
 *
 * This service fails if @a sm is an unnamed semaphore.
 *
 * @param sm the semaphore to be closed.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EINVAL, the semaphore @a sm is invalid or is an unnamed semaphore.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_close.html">
 * Specification.</a>
 * 
 */
int sem_close (sem_t *sm)
{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    nsem_t *named_sem;
    spl_t s;
    int err;

    xnlock_get_irqsave(&nklock, s);

    if(shadow->magic != PSE51_NAMED_SEM_MAGIC)
        {
        err = EINVAL;
        goto error;
        }

    named_sem = sem2named_sem(shadow->sem);
    
    err = pse51_node_put(&named_sem->nodebase);

    if (err)
        goto error;
    
    if (pse51_node_removed_p(&named_sem->nodebase))
        {
        /* unlink was called, and this semaphore is no longer referenced. */
        sem_destroy_internal(&named_sem->sembase);
        pse51_mark_deleted(shadow);
        }
    else if (!pse51_node_ref_p(&named_sem->nodebase))
        /* this semaphore is no longer referenced, but not unlinked. */
        pse51_mark_deleted(shadow);

    xnlock_put_irqrestore(&nklock, s);

    return 0;

  error:
    xnlock_put_irqrestore(&nklock, s);

    thread_set_errno(err);

    return -1;
}

/**
 * Unlink a named semaphore.
 *
 * This service unlinks the semaphore named @a name. This semaphore is not
 * destroyed until all references obtained with sem_open() are closed by calling
 * sem_close(). However, the unlinked semaphore may no longer be reached with
 * the sem_open() service.
 *
 * When a semaphore is destroyed, the memory it used is returned to the system
 * heap, so that further references to this semaphore are not guaranteed to
 * fail, as is the case for unnamed semaphores.
 *
 * @param name the name of the semaphore to be unlinked.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - ENAMETOOLONG, the length of the @a name argument exceeds 64 characters;
 * - ENOENT, the named semaphore does not exist.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_unlink.html">
 * Specification.</a>
 * 
 */
int sem_unlink (const char *name)
{
    pse51_node_t *node;
    nsem_t *named_sem;
    spl_t s;
    int err;

    xnlock_get_irqsave(&nklock, s);

    err = pse51_node_remove(&node, name, PSE51_NAMED_SEM_MAGIC);

    if (err)
        goto error;

    named_sem = node2sem(node);
    
    if (pse51_node_removed_p(&named_sem->nodebase))
        sem_destroy_internal(&named_sem->sembase);

    xnlock_put_irqrestore(&nklock, s);

    return 0;

  error:
    xnlock_put_irqrestore(&nklock, s);

    thread_set_errno(err);

    return -1;
}

static inline int sem_trywait_internal (struct __shadow_sem *shadow)

{
    pse51_sem_t *sem;

    if (shadow->magic != PSE51_SEM_MAGIC
        && shadow->magic != PSE51_NAMED_SEM_MAGIC)
        return EINVAL;

    sem = shadow->sem;
 
    if (sem->value == 0)
        return EAGAIN;

    --sem->value;

    return 0;
}

/**
 * Attempt to lock a semaphore.
 *
 * This service is equivalent to sem_wait(), except that it returns immediately
 * if the semaphore @a sm is currently locked, and that it is not a cancellation
 * point.
 *
 * @param sm the semaphore to be locked.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EINVAL, the specified semaphore is invalid or uninitialized;
 * - EAGAIN, the specified semaphore is currently locked.
 *
 * * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_trywait.html">
 * Specification.</a>
 * 
 */
int sem_trywait (sem_t *sm)

{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    int err;
    spl_t s;
    
    xnlock_get_irqsave(&nklock, s);

    err = sem_trywait_internal(shadow);

    xnlock_put_irqrestore(&nklock, s);
    
    if (err)
        {
        thread_set_errno(err);
        return -1;
        }

    return 0;
}

static inline int sem_timedwait_internal (struct __shadow_sem *shadow,
                                          xnticks_t to)
{
    pse51_sem_t *sem = shadow->sem;
    xnthread_t *cur;
    int err;

    if (xnpod_unblockable_p())
        return EPERM;

    cur = xnpod_current_thread();

    if ((err = sem_trywait_internal(shadow)) != EAGAIN)
        return err;

    if((err = clock_adjust_timeout(&to, CLOCK_REALTIME)))
        return err;

    thread_cancellation_point(cur);

    xnsynch_sleep_on(&sem->synchbase, to);
            
    /* Handle cancellation requests. */
    thread_cancellation_point(cur);

    if (xnthread_test_flags(cur, XNRMID))
        return EINVAL;
    
    if (xnthread_test_flags(cur, XNBREAK))
        return EINTR;
    
    if (xnthread_test_flags(cur, XNTIMEO))
        return ETIMEDOUT;

    return 0;
}

/**
 * Lock a semaphore.
 *
 * This service locks the semaphore @a sm if it is currently unlocked (i.e. if
 * its value is greater than 0). If the semaphore is currently locked, the
 * calling thread is suspended until the semaphore is unlocked, or a signal is
 * delivered to the calling thread.
 *
 * This service is a cancellation point for Xenomai POSIX skin threads (created
 * with the pthread_create() service). When such a thread is cancelled while
 * blocked in a call to this service, the semaphore state is left unchanged
 * before the cancellation cleanup handlers are called.
 *
 * @param sm the semaphore to be locked.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the semaphore is invalid or uninitialized;
 * - EINTR, the caller was interrupted by a signal while blocked in this
 *   service.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread,
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_wait.html">
 * Specification.</a>
 * 
 */
int sem_wait (sem_t *sm)

{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    spl_t s;
    int err;

    xnlock_get_irqsave(&nklock, s);
    err = sem_timedwait_internal(shadow, XN_INFINITE);
    xnlock_put_irqrestore(&nklock, s);

    if(err)
        {
        thread_set_errno(err);
        return -1;
        }
    
    return 0;
}

/**
 * Attempt, during a bounded time, to lock a semaphore.
 *
 * This serivce is equivalent to sem_wait(), except that the caller is only
 * blocked until the timeout @a abs_timeout expires.
 *
 * @param sm the semaphore to be locked;
 *
 * @param abs_timeout the timeout, expressed as an absolute value of the
 * CLOCK_REALTIME clock.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the semaphore is invalid or uninitialized;
 * - EINVAL, the specified timeout is invalid;
 * - EINTR, the caller was interrupted by a signal while blocked in this
 *   service;
 * - ETIMEDOUT, the semaphore could not be locked and the specified timeout
 *   expired.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread,
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_timedwait.html">
 * Specification.</a>
 * 
 */
int sem_timedwait (sem_t *sm, const struct timespec *abs_timeout)

{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    spl_t s;
    int err;

    xnlock_get_irqsave(&nklock, s);
    err = sem_timedwait_internal(shadow, ts2ticks_ceil(abs_timeout)+1);
    xnlock_put_irqrestore(&nklock, s);

    if(err)
        {
        thread_set_errno(err);
        return -1;
        }
    
    return 0;
}

/**
 * Unlock a semaphore.
 *
 * This service unlocks the semaphore @a sm.
 *
 * If no thread is currently blocked on this semaphore, its count is
 * incremented, otherwise the highest priority thread is unblocked.
 *
 * @param sm the semaphore to be unlocked.
 *
 * @retval 0 on success;
 * @retval -1 with errno set if:
 * - EINVAL, the specified semaphore is invalid or uninitialized;
 * - EAGAIN, the semaphore count is @a SEM_VALUE_MAX.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_post.html">
 * Specification.</a>
 * 
 */
int sem_post (sem_t *sm)

{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    pse51_sem_t *sem;
    spl_t s;
    
    xnlock_get_irqsave(&nklock, s);

    if (shadow->magic != PSE51_SEM_MAGIC
        && shadow->magic != PSE51_NAMED_SEM_MAGIC)
        {
        thread_set_errno(EINVAL);
        goto error;
        }

    sem = shadow->sem;

    if (sem->value == SEM_VALUE_MAX)
        {
        thread_set_errno(EAGAIN);
        goto error;
        }

    if(xnsynch_wakeup_one_sleeper(&sem->synchbase) != NULL)
        xnpod_schedule();
    else
        ++sem->value;

    xnlock_put_irqrestore(&nklock, s);

    return 0;

 error:

    xnlock_put_irqrestore(&nklock, s);

    return -1;
}

/**
 * Get the value of a semaphore.
 *
 * This service stores at the address @a value, the current count of the
 * semaphore @a sm. The state of the semaphore is unchanged.
 *
 * If the semaphore is currently locked, the value stored is zero.
 *
 * @param sm a semaphore;
 *
 * @param value address where the semaphore count will be stored on success.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EINVAL, the semaphore is invalid or uninitialized.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/sem_getvalue.html">
 * Specification.</a>
 * 
 */
int sem_getvalue (sem_t *sm, int *value)

{
    struct __shadow_sem *shadow = &((union __xeno_sem *) sm)->shadow_sem;
    pse51_sem_t *sem;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (shadow->magic != PSE51_SEM_MAGIC
        && shadow->magic != PSE51_NAMED_SEM_MAGIC)
        {
        xnlock_put_irqrestore(&nklock, s);
        thread_set_errno(EINVAL);
        return -1;
        }

    sem = shadow->sem;

    *value = sem->value;

    xnlock_put_irqrestore(&nklock, s);

    return 0;
}

#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)
pse51_assocq_t pse51_usems;
#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */

void pse51_sem_pkg_init (void) {

    initq(&pse51_semq);
#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)
    pse51_assocq_init(&pse51_usems);
#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */
}

void pse51_sem_pkg_cleanup (void)

{
    xnholder_t *holder;
    spl_t s;

#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)
    pse51_assocq_destroy(&pse51_usems, NULL);
#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */

    xnlock_get_irqsave(&nklock, s);

    while ((holder = getheadq(&pse51_semq)) != NULL)
        {
        pse51_sem_t *sem = link2sem(holder);

#ifdef CONFIG_XENO_OPT_DEBUG
        if (sem->magic == PSE51_SEM_MAGIC)
            xnprintf("POSIX semaphore %p discarded.\n",
                     sem);
        else
            xnprintf("POSIX semaphore \"%s\" discarded.\n",
                     sem2named_sem(sem)->nodebase.name);
#endif /* CONFIG_XENO_OPT_DEBUG */
        sem_destroy_internal(sem);
        }

    xnlock_put_irqrestore(&nklock, s);
}

/*@}*/

EXPORT_SYMBOL(pse51_sem_init);
EXPORT_SYMBOL(sem_destroy);
EXPORT_SYMBOL(sem_post);
EXPORT_SYMBOL(sem_trywait);
EXPORT_SYMBOL(sem_wait);
EXPORT_SYMBOL(sem_timedwait);
EXPORT_SYMBOL(sem_getvalue);
EXPORT_SYMBOL(sem_open);
EXPORT_SYMBOL(sem_close);
EXPORT_SYMBOL(sem_unlink);
