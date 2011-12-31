/**
 *   @ingroup hal
 *   @file
 *
 *   Adeos-based Real-Time Abstraction Layer for x86.
 *   Common code of i386 and x86_64.
 *
 *   Copyright (C) 2007 Philippe Gerum.
 *
 *   Xenomai is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation, Inc., 675 Mass Ave,
 *   Cambridge MA 02139, USA; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   Xenomai is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *   02111-1307, USA.
 */

/**
 * @addtogroup hal
 *
 *@{*/

#include <linux/module.h>
#include <linux/ipipe_tickdev.h>
#include <asm/xenomai/hal.h>

enum rthal_ktimer_mode rthal_ktimer_saved_mode;

#ifdef CONFIG_X86_LOCAL_APIC

static volatile int sync_op;

#define RTHAL_SET_ONESHOT_XENOMAI	1
#define RTHAL_SET_ONESHOT_LINUX		2
#define RTHAL_SET_PERIODIC		3

static void critical_sync(void)
{
	if (!rthal_cpu_supported(ipipe_processor_id()))
		return;
	switch (sync_op) {
	case RTHAL_SET_ONESHOT_XENOMAI:
		rthal_setup_oneshot_apic(RTHAL_APIC_TIMER_VECTOR);
		break;

	case RTHAL_SET_ONESHOT_LINUX:
		rthal_setup_oneshot_apic(LOCAL_TIMER_VECTOR);
		/* We need to keep the timing cycle alive for the kernel. */
		ipipe_raise_irq(RTHAL_HOST_TICK_IRQ);
		break;

	case RTHAL_SET_PERIODIC:
		rthal_setup_periodic_apic(RTHAL_APIC_ICOUNT, LOCAL_TIMER_VECTOR);
		break;
	}
}

static void rthal_timer_set_oneshot(int rt_mode)
{
	unsigned long flags;

	flags = ipipe_critical_enter(critical_sync);
	if (rt_mode) {
		sync_op = RTHAL_SET_ONESHOT_XENOMAI;
		if (rthal_cpu_supported(ipipe_processor_id()))
			rthal_setup_oneshot_apic(RTHAL_APIC_TIMER_VECTOR);
		if (rthal_ktimer_saved_mode != KTIMER_MODE_UNUSED)
			__ipipe_hrtimer_irq = RTHAL_TIMER_IRQ;
	} else {
		sync_op = RTHAL_SET_ONESHOT_LINUX;
		if (rthal_cpu_supported(ipipe_processor_id()))
			rthal_setup_oneshot_apic(LOCAL_TIMER_VECTOR);
		__ipipe_hrtimer_irq = RTHAL_HOST_TICK_IRQ;
		/* We need to keep the timing cycle alive for the kernel. */
		ipipe_raise_irq(RTHAL_HOST_TICK_IRQ);
	}
	ipipe_critical_exit(flags);
}

static void rthal_timer_set_periodic(void)
{
	unsigned long flags;

	flags = ipipe_critical_enter(critical_sync);
	sync_op = RTHAL_SET_PERIODIC;
	if (rthal_cpu_supported(ipipe_processor_id()))
		rthal_setup_periodic_apic(RTHAL_APIC_ICOUNT, LOCAL_TIMER_VECTOR);
	__ipipe_hrtimer_irq = RTHAL_HOST_TICK_IRQ;
	ipipe_critical_exit(flags);
}

static int cpu_timers_requested;

int rthal_timer_request(
	void (*tick_handler)(void),
	void (*mode_emul)(enum clock_event_mode mode,
			  struct clock_event_device *cdev),
	int (*tick_emul)(unsigned long delay,
			 struct clock_event_device *cdev),
	int cpu)
{
	unsigned long dummy, *tmfreq = &dummy;
	int tickval, ret, res;

	if (cpu_timers_requested == 0) {
		ret = ipipe_request_irq(&rthal_archdata.domain,
					RTHAL_APIC_TIMER_IPI,
					(ipipe_irq_handler_t)tick_handler,
					NULL, NULL);
		if (ret)
			return ret;
	}

	/* This code works both for UP+LAPIC and SMP configurations. */

	if (rthal_timerfreq_arg == 0)
		tmfreq = &rthal_archdata.timer_freq;

	res = ipipe_request_tickdev("lapic", mode_emul, tick_emul, cpu,
				    tmfreq);
	switch (res) {
	case CLOCK_EVT_MODE_PERIODIC:
		/* oneshot tick emulation callback won't be used, ask
		 * the caller to start an internal timer for emulating
		 * a periodic tick. */
		tickval = 1000000000UL / HZ;
		break;

	case CLOCK_EVT_MODE_ONESHOT:
		/* oneshot tick emulation */
		tickval = 1;
		break;

	case CLOCK_EVT_MODE_UNUSED:
		/* we don't need to emulate the tick at all. */
		tickval = 0;
		break;

	case CLOCK_EVT_MODE_SHUTDOWN:
		res = -ENODEV;
		/* fall through */

	default:
		if (cpu_timers_requested == 0)
			ipipe_free_irq(&rthal_archdata.domain,
				       RTHAL_APIC_TIMER_IPI);
		return res;
	}
	rthal_ktimer_saved_mode = res;

	/*
	 * The rest of the initialization should only be performed
	 * once by a single CPU.
	 */
	if (cpu_timers_requested++ == 0)
		rthal_timer_set_oneshot(1);

	return tickval;
}

void rthal_timer_release(int cpu)
{
	ipipe_release_tickdev(cpu);

	/*
	 * The rest of the cleanup work should only be performed once
	 * by the last releasing CPU.
	 */
	if (--cpu_timers_requested > 0)
		return;

	if (rthal_ktimer_saved_mode == KTIMER_MODE_PERIODIC)
		rthal_timer_set_periodic();
	else if (rthal_ktimer_saved_mode == KTIMER_MODE_ONESHOT)
		rthal_timer_set_oneshot(0);

	ipipe_free_irq(&rthal_archdata.domain, RTHAL_APIC_TIMER_IPI);
}

#endif /* CONFIG_X86_LOCAL_APIC */

void rthal_timer_notify_switch(enum clock_event_mode mode,
			       struct clock_event_device *cdev)
{
	if (ipipe_processor_id() > 0)
		/*
		 * We assume all CPUs switch the same way, so we only
		 * track mode switches from the boot CPU.
		 */
		return;

	rthal_ktimer_saved_mode = mode;
}
EXPORT_SYMBOL_GPL(rthal_timer_notify_switch);

/*@}*/