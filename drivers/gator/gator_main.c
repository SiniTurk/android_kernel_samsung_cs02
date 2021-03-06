/**
 * Copyright (C) ARM Limited 2010-2013. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

// This version must match the gator daemon version
static unsigned long gator_protocol_version = 14;

#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/irq.h>
#include <linux/vmalloc.h>
#include <linux/hardirq.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/utsname.h>
#include <asm/stacktrace.h>
#include <asm/uaccess.h>

#include "gator.h"
#include "gator_events.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
#error kernels prior to 2.6.32 are not supported
#endif

#if !defined(CONFIG_GENERIC_TRACER) && !defined(CONFIG_TRACING)
#error gator requires the kernel to have CONFIG_GENERIC_TRACER or CONFIG_TRACING defined
#endif

#ifndef CONFIG_PROFILING
#error gator requires the kernel to have CONFIG_PROFILING defined
#endif

#ifndef CONFIG_HIGH_RES_TIMERS
#error gator requires the kernel to have CONFIG_HIGH_RES_TIMERS defined to support PC sampling
#endif

#if defined(__arm__) && defined(CONFIG_SMP) && !defined(CONFIG_LOCAL_TIMERS)
#error gator requires the kernel to have CONFIG_LOCAL_TIMERS defined on SMP systems
#endif

#if (GATOR_PERF_SUPPORT) && (!(GATOR_PERF_PMU_SUPPORT))
#ifndef CONFIG_PERF_EVENTS
#error gator requires the kernel to have CONFIG_PERF_EVENTS defined to support pmu hardware counters
#elif !defined CONFIG_HW_PERF_EVENTS
#error gator requires the kernel to have CONFIG_HW_PERF_EVENTS defined to support pmu hardware counters
#endif
#endif

/******************************************************************************
 * DEFINES
 ******************************************************************************/
#define SUMMARY_BUFFER_SIZE       (1*1024)
#define BACKTRACE_BUFFER_SIZE     (128*1024)
#define NAME_BUFFER_SIZE          (64*1024)
#define COUNTER_BUFFER_SIZE       (64*1024)	// counters have the core as part of the data and the core value in the frame header may be discarded
#define BLOCK_COUNTER_BUFFER_SIZE (128*1024)
#define ANNOTATE_BUFFER_SIZE      (64*1024)	// annotate counters have the core as part of the data and the core value in the frame header may be discarded
#define SCHED_TRACE_BUFFER_SIZE   (128*1024)
#define GPU_TRACE_BUFFER_SIZE     (64*1024)	// gpu trace counters have the core as part of the data and the core value in the frame header may be discarded
#define IDLE_BUFFER_SIZE          (32*1024)	// idle counters have the core as part of the data and the core value in the frame header may be discarded

#define NO_COOKIE      0U
#define INVALID_COOKIE ~0U

#define FRAME_SUMMARY       1
#define FRAME_BACKTRACE     2
#define FRAME_NAME          3
#define FRAME_COUNTER       4
#define FRAME_BLOCK_COUNTER 5
#define FRAME_ANNOTATE      6
#define FRAME_SCHED_TRACE   7
#define FRAME_GPU_TRACE     8
#define FRAME_IDLE          9

#define MESSAGE_END_BACKTRACE 1

#define MESSAGE_COOKIE      1
#define MESSAGE_THREAD_NAME 2
#define HRTIMER_CORE_NAME   3

#define MESSAGE_GPU_START 1
#define MESSAGE_GPU_STOP  2

#define MESSAGE_SCHED_SWITCH 1
#define MESSAGE_SCHED_EXIT   2

#define MESSAGE_IDLE_ENTER 1
#define MESSAGE_IDLE_EXIT 2

#define MAXSIZE_PACK32     5
#define MAXSIZE_PACK64    10

#define FRAME_HEADER_SIZE 3

#if defined(__arm__)
#define PC_REG regs->ARM_pc
#elif defined(__aarch64__)
#define PC_REG regs->pc
#else
#define PC_REG regs->ip
#endif

enum {
	SUMMARY_BUF,
	BACKTRACE_BUF,
	NAME_BUF,
	COUNTER_BUF,
	BLOCK_COUNTER_BUF,
	ANNOTATE_BUF,
	SCHED_TRACE_BUF,
	GPU_TRACE_BUF,
	IDLE_BUF,
	NUM_GATOR_BUFS
};

/******************************************************************************
 * Globals
 ******************************************************************************/
static unsigned long gator_cpu_cores;
// Size of the largest buffer. Effectively constant, set in gator_op_create_files
static unsigned long userspace_buffer_size;
static unsigned long gator_backtrace_depth;
// How often to commit the buffers for live in nanoseconds
static u64 gator_live_rate;

static unsigned long gator_started;
static u64 gator_monotonic_started;
static unsigned long gator_buffer_opened;
static unsigned long gator_timer_count;
static unsigned long gator_response_type;
static DEFINE_MUTEX(start_mutex);
static DEFINE_MUTEX(gator_buffer_mutex);

bool event_based_sampling;

static DECLARE_WAIT_QUEUE_HEAD(gator_buffer_wait);
static DECLARE_WAIT_QUEUE_HEAD(gator_annotate_wait);
static struct timer_list gator_buffer_wake_up_timer;
static LIST_HEAD(gator_events);

static DEFINE_PER_CPU(u64, last_timestamp);

static bool printed_monotonic_warning;

static bool sent_core_name[NR_CPUS];

/******************************************************************************
 * Prototypes
 ******************************************************************************/
static void buffer_check(int cpu, int buftype, u64 time);
static void gator_commit_buffer(int cpu, int buftype, u64 time);
static int buffer_bytes_available(int cpu, int buftype);
static bool buffer_check_space(int cpu, int buftype, int bytes);
static int contiguous_space_available(int cpu, int bufytpe);
static void gator_buffer_write_packed_int(int cpu, int buftype, int x);
static void gator_buffer_write_packed_int64(int cpu, int buftype, long long x);
static void gator_buffer_write_bytes(int cpu, int buftype, const char *x, int len);
static void gator_buffer_write_string(int cpu, int buftype, const char *x);
static void gator_add_trace(int cpu, unsigned long address);
static void gator_add_sample(int cpu, struct pt_regs *const regs);
static u64 gator_get_time(void);

// Size of the buffer, must be a power of 2. Effectively constant, set in gator_op_setup.
static uint32_t gator_buffer_size[NUM_GATOR_BUFS];
// gator_buffer_size - 1, bitwise and with pos to get offset into the array. Effectively constant, set in gator_op_setup.
static uint32_t gator_buffer_mask[NUM_GATOR_BUFS];
// Read position in the buffer. Initialized to zero in gator_op_setup and incremented after bytes are read by userspace in userspace_buffer_read
static DEFINE_PER_CPU(int[NUM_GATOR_BUFS], gator_buffer_read);
// Write position in the buffer. Initialized to zero in gator_op_setup and incremented after bytes are written to the buffer
static DEFINE_PER_CPU(int[NUM_GATOR_BUFS], gator_buffer_write);
// Commit position in the buffer. Initialized to zero in gator_op_setup and incremented after a frame is ready to be read by userspace
static DEFINE_PER_CPU(int[NUM_GATOR_BUFS], gator_buffer_commit);
// If set to false, decreases the number of bytes returned by buffer_bytes_available. Set in buffer_check_space if no space is remaining. Initialized to true in gator_op_setup
// This means that if we run out of space, continue to report that no space is available until bytes are read by userspace
static DEFINE_PER_CPU(int[NUM_GATOR_BUFS], buffer_space_available);
// The buffer. Allocated in gator_op_setup
static DEFINE_PER_CPU(char *[NUM_GATOR_BUFS], gator_buffer);

#if GATOR_LIVE
// The time after which the buffer should be committed for live display
static DEFINE_PER_CPU(u64, gator_buffer_commit_time);
#endif

/******************************************************************************
 * Application Includes
 ******************************************************************************/
#include "gator_marshaling.c"
#include "gator_hrtimer_perf.c"
#include "gator_hrtimer_gator.c"
#include "gator_cookies.c"
#include "gator_trace_sched.c"
#include "gator_trace_power.c"
#include "gator_trace_gpu.c"
#include "gator_backtrace.c"
#include "gator_annotate.c"
#include "gator_fs.c"
#include "gator_pack.c"

/******************************************************************************
 * Misc
 ******************************************************************************/

const struct gator_cpu gator_cpus[] = {
	{
		.cpuid = ARM1136,
		.core_name = "ARM1136",
		.pmnc_name = "ARM_ARM11",
		.pmnc_counters = 3,
	},
	{
		.cpuid = ARM1156,
		.core_name = "ARM1156",
		.pmnc_name = "ARM_ARM11",
		.pmnc_counters = 3,
	},
	{
		.cpuid = ARM1176,
		.core_name = "ARM1176",
		.pmnc_name = "ARM_ARM11",
		.pmnc_counters = 3,
	},
	{
		.cpuid = ARM11MPCORE,
		.core_name = "ARM11MPCore",
		.pmnc_name = "ARM_ARM11MPCore",
		.pmnc_counters = 3,
	},
	{
		.cpuid = CORTEX_A5,
		.core_name = "Cortex-A5",
		.pmu_name = "ARMv7_Cortex_A5",
		.pmnc_name = "ARM_Cortex-A5",
		.pmnc_counters = 2,
	},
	{
		.cpuid = CORTEX_A7,
		.core_name = "Cortex-A7",
		.pmu_name = "ARMv7_Cortex_A7",
		.pmnc_name = "ARM_Cortex-A7",
		.pmnc_counters = 4,
	},
	{
		.cpuid = CORTEX_A8,
		.core_name = "Cortex-A8",
		.pmu_name = "ARMv7_Cortex_A8",
		.pmnc_name = "ARM_Cortex-A8",
		.pmnc_counters = 4,
	},
	{
		.cpuid = CORTEX_A9,
		.core_name = "Cortex-A9",
		.pmu_name = "ARMv7_Cortex_A9",
		.pmnc_name = "ARM_Cortex-A9",
		.pmnc_counters = 6,
	},
	{
		.cpuid = CORTEX_A15,
		.core_name = "Cortex-A15",
		.pmu_name = "ARMv7_Cortex_A15",
		.pmnc_name = "ARM_Cortex-A15",
		.pmnc_counters = 6,
	},
	{
		.cpuid = SCORPION,
		.core_name = "Scorpion",
		.pmnc_name = "Scorpion",
		.pmnc_counters = 4,
	},
	{
		.cpuid = SCORPIONMP,
		.core_name = "ScorpionMP",
		.pmnc_name = "ScorpionMP",
		.pmnc_counters = 4,
	},
	{
		.cpuid = KRAITSIM,
		.core_name = "KraitSIM",
		.pmnc_name = "Krait",
		.pmnc_counters = 4,
	},
	{
		.cpuid = KRAIT,
		.core_name = "Krait",
		.pmnc_name = "Krait",
		.pmnc_counters = 4,
	},
	{
		.cpuid = KRAIT_S4_PRO,
		.core_name = "Krait S4 Pro",
		.pmnc_name = "Krait",
		.pmnc_counters = 4,
	},
	{
		.cpuid = CORTEX_A53,
		.core_name = "Cortex-A53",
		.pmnc_name = "ARM_Cortex-A53",
		.pmnc_counters = 6,
	},
	{
		.cpuid = CORTEX_A57,
		.core_name = "Cortex-A57",
		.pmnc_name = "ARM_Cortex-A57",
		.pmnc_counters = 6,
	},
	{
		.cpuid = AARCH64,
		.core_name = "AArch64",
		.pmnc_name = "ARM_AArch64",
		.pmnc_counters = 6,
	},
	{
		.cpuid = OTHER,
		.core_name = "Other",
		.pmnc_name = "Other",
		.pmnc_counters = 6,
	},
	{}
};

const struct gator_cpu *gator_find_cpu_by_cpuid(const u32 cpuid)
{
	int i;

	for (i = 0; gator_cpus[i].cpuid != 0; ++i) {
		const struct gator_cpu *const gator_cpu = &gator_cpus[i];
		if (gator_cpu->cpuid == cpuid) {
			return gator_cpu;
		}
	}

	return NULL;
}

const struct gator_cpu *gator_find_cpu_by_pmu_name(const char *const name)
{
	int i;

	for (i = 0; gator_cpus[i].cpuid != 0; ++i) {
		const struct gator_cpu *const gator_cpu = &gator_cpus[i];
		if (gator_cpu->pmu_name != NULL && strcmp(gator_cpu->pmu_name, name) == 0) {
			return gator_cpu;
		}
	}

	return NULL;
}

u32 gator_cpuid(void)
{
#if defined(__arm__) || defined(__aarch64__)
	u32 val;
#if !defined(__aarch64__)
	asm volatile("mrc p15, 0, %0, c0, c0, 0" : "=r" (val));
#else
	asm volatile("mrs %0, midr_el1" : "=r" (val));
#endif
	return (val >> 4) & 0xfff;
#else
	return OTHER;
#endif
}

static void gator_buffer_wake_up(unsigned long data)
{
	wake_up(&gator_buffer_wait);
}

/******************************************************************************
 * Commit interface
 ******************************************************************************/
static bool buffer_commit_ready(int *cpu, int *buftype)
{
	int cpu_x, x;
	for_each_present_cpu(cpu_x) {
		for (x = 0; x < NUM_GATOR_BUFS; x++)
			if (per_cpu(gator_buffer_commit, cpu_x)[x] != per_cpu(gator_buffer_read, cpu_x)[x]) {
				*cpu = cpu_x;
				*buftype = x;
				return true;
			}
	}
	*cpu = -1;
	*buftype = -1;
	return false;
}

/******************************************************************************
 * Buffer management
 ******************************************************************************/
static int buffer_bytes_available(int cpu, int buftype)
{
	int remaining, filled;

	filled = per_cpu(gator_buffer_write, cpu)[buftype] - per_cpu(gator_buffer_read, cpu)[buftype];
	if (filled < 0) {
		filled += gator_buffer_size[buftype];
	}

	remaining = gator_buffer_size[buftype] - filled;

	if (per_cpu(buffer_space_available, cpu)[buftype]) {
		// Give some extra room; also allows space to insert the overflow error packet
		remaining -= 200;
	} else {
		// Hysteresis, prevents multiple overflow messages
		remaining -= 2000;
	}

	return remaining;
}

static int contiguous_space_available(int cpu, int buftype)
{
	int remaining = buffer_bytes_available(cpu, buftype);
	int contiguous = gator_buffer_size[buftype] - per_cpu(gator_buffer_write, cpu)[buftype];
	if (remaining < contiguous)
		return remaining;
	else
		return contiguous;
}

static bool buffer_check_space(int cpu, int buftype, int bytes)
{
	int remaining = buffer_bytes_available(cpu, buftype);

	if (remaining < bytes) {
		per_cpu(buffer_space_available, cpu)[buftype] = false;
	} else {
		per_cpu(buffer_space_available, cpu)[buftype] = true;
	}

	return per_cpu(buffer_space_available, cpu)[buftype];
}

static void gator_buffer_write_bytes(int cpu, int buftype, const char *x, int len)
{
	int i;
	u32 write = per_cpu(gator_buffer_write, cpu)[buftype];
	u32 mask = gator_buffer_mask[buftype];
	char *buffer = per_cpu(gator_buffer, cpu)[buftype];

	for (i = 0; i < len; i++) {
		buffer[write] = x[i];
		write = (write + 1) & mask;
	}

	per_cpu(gator_buffer_write, cpu)[buftype] = write;
}

static void gator_buffer_write_string(int cpu, int buftype, const char *x)
{
	int len = strlen(x);
	gator_buffer_write_packed_int(cpu, buftype, len);
	gator_buffer_write_bytes(cpu, buftype, x, len);
}

static void gator_commit_buffer(int cpu, int buftype, u64 time)
{
	int type_length, commit, length, byte;

	if (!per_cpu(gator_buffer, cpu)[buftype])
		return;

	// post-populate the length, which does not include the response type length nor the length itself, i.e. only the length of the payload
	type_length = gator_response_type ? 1 : 0;
	commit = per_cpu(gator_buffer_commit, cpu)[buftype];
	length = per_cpu(gator_buffer_write, cpu)[buftype] - commit;
	if (length < 0) {
		length += gator_buffer_size[buftype];
	}
	length = length - type_length - sizeof(s32);

	if (length <= FRAME_HEADER_SIZE) {
		// Nothing to write, only the frame header is present
		return;
	}

	for (byte = 0; byte < sizeof(s32); byte++) {
		per_cpu(gator_buffer, cpu)[buftype][(commit + type_length + byte) & gator_buffer_mask[buftype]] = (length >> byte * 8) & 0xFF;
	}

	per_cpu(gator_buffer_commit, cpu)[buftype] = per_cpu(gator_buffer_write, cpu)[buftype];

#if GATOR_LIVE
	if (gator_live_rate > 0) {
		while (time > per_cpu(gator_buffer_commit_time, cpu)) {
			per_cpu(gator_buffer_commit_time, cpu) += gator_live_rate;
		}
	}
#endif

	marshal_frame(cpu, buftype);

	// had to delay scheduling work as attempting to schedule work during the context switch is illegal in kernel versions 3.5 and greater
	mod_timer(&gator_buffer_wake_up_timer, jiffies + 1);
}

static void buffer_check(int cpu, int buftype, u64 time)
{
	int filled = per_cpu(gator_buffer_write, cpu)[buftype] - per_cpu(gator_buffer_commit, cpu)[buftype];
	if (filled < 0) {
		filled += gator_buffer_size[buftype];
	}
	if (filled >= ((gator_buffer_size[buftype] * 3) / 4)) {
		gator_commit_buffer(cpu, buftype, time);
	}
}

static void gator_add_trace(int cpu, unsigned long address)
{
	off_t offset = 0;
	unsigned long cookie = get_address_cookie(cpu, current, address & ~1, &offset);

	if (cookie == NO_COOKIE || cookie == INVALID_COOKIE) {
		offset = address;
	}

	marshal_backtrace(offset & ~1, cookie);
}

static void gator_add_sample(int cpu, struct pt_regs *const regs)
{
	bool inKernel;
	unsigned long exec_cookie;

	if (!regs)
		return;

	inKernel = !user_mode(regs);
	exec_cookie = get_exec_cookie(cpu, current);

	if (!marshal_backtrace_header(exec_cookie, current->tgid, current->pid, inKernel))
		return;

	if (inKernel) {
		kernel_backtrace(cpu, regs);
	} else {
		// Cookie+PC
		gator_add_trace(cpu, PC_REG);

		// Backtrace
		if (gator_backtrace_depth)
			arm_backtrace_eabi(cpu, regs, gator_backtrace_depth);
	}

	marshal_backtrace_footer();
}

/******************************************************************************
 * hrtimer interrupt processing
 ******************************************************************************/
static void gator_timer_interrupt(void)
{
	struct pt_regs *const regs = get_irq_regs();
	gator_backtrace_handler(regs);
}

void gator_backtrace_handler(struct pt_regs *const regs)
{
	int cpu = get_physical_cpu();

	// Output backtrace
	gator_add_sample(cpu, regs);

	// Collect counters
	if (!per_cpu(collecting, cpu)) {
		collect_counters();
	}
}

static int gator_running;

// This function runs in interrupt context and on the appropriate core
static void gator_timer_offline(void *migrate)
{
	struct gator_interface *gi;
	int i, len, cpu = get_physical_cpu();
	int *buffer;
	u64 time;

	gator_trace_sched_offline();
	gator_trace_power_offline();

	if (!migrate) {
		gator_hrtimer_offline();
	}

	// Offline any events and output counters
	time = gator_get_time();
	if (marshal_event_header(time)) {
		list_for_each_entry(gi, &gator_events, list) {
			if (gi->offline) {
				len = gi->offline(&buffer, migrate);
				marshal_event(len, buffer);
			}
		}
		// Only check after writing all counters so that time and corresponding counters appear in the same frame
		buffer_check(cpu, BLOCK_COUNTER_BUF, time);
	}

	// Flush all buffers on this core
	for (i = 0; i < NUM_GATOR_BUFS; i++)
		gator_commit_buffer(cpu, i, time);
}

// This function runs in interrupt context and may be running on a core other than core 'cpu'
static void gator_timer_offline_dispatch(int cpu, bool migrate)
{
	struct gator_interface *gi;

	list_for_each_entry(gi, &gator_events, list) {
		if (gi->offline_dispatch) {
			gi->offline_dispatch(cpu, migrate);
		}
	}
}

static void gator_timer_stop(void)
{
	int cpu;

	if (gator_running) {
		on_each_cpu(gator_timer_offline, NULL, 1);
		for_each_online_cpu(cpu) {
			gator_timer_offline_dispatch(lcpu_to_pcpu(cpu), false);
		}

		gator_running = 0;
		gator_hrtimer_shutdown();
	}
}

// This function runs in interrupt context and on the appropriate core
static void gator_timer_online(void *migrate)
{
	struct gator_interface *gi;
	int len, cpu = get_physical_cpu();
	int *buffer;
	u64 time;

	gator_trace_power_online();

	// online any events and output counters
	time = gator_get_time();
	if (marshal_event_header(time)) {
		list_for_each_entry(gi, &gator_events, list) {
			if (gi->online) {
				len = gi->online(&buffer, migrate);
				marshal_event(len, buffer);
			}
		}
		// Only check after writing all counters so that time and corresponding counters appear in the same frame
		buffer_check(cpu, BLOCK_COUNTER_BUF, time);
	}

	if (!migrate) {
		gator_hrtimer_online();
	}

#if defined(__arm__) || defined(__aarch64__)
	if (!sent_core_name[cpu]) {
		const char *core_name = NULL;
		const u32 cpuid = gator_cpuid();
		const struct gator_cpu *const gator_cpu = gator_find_cpu_by_cpuid(cpuid);
		char core_name_buf[32];

		if (gator_cpu != NULL) {
			core_name = gator_cpu->core_name;
		} else {
			snprintf(core_name_buf, sizeof(core_name_buf), "Unknown (0x%.3x)", cpuid);
			core_name = core_name_buf;
		}

		marshal_core_name(cpuid, core_name);
		sent_core_name[cpu] = true;
	}
#endif
}

// This function runs in interrupt context and may be running on a core other than core 'cpu'
static void gator_timer_online_dispatch(int cpu, bool migrate)
{
	struct gator_interface *gi;

	list_for_each_entry(gi, &gator_events, list) {
		if (gi->online_dispatch) {
			gi->online_dispatch(cpu, migrate);
		}
	}
}

int gator_timer_start(unsigned long sample_rate)
{
	int cpu;

	if (gator_running) {
		pr_notice("gator: already running\n");
		return 0;
	}

	gator_running = 1;

	// event based sampling trumps hr timer based sampling
	if (event_based_sampling) {
		sample_rate = 0;
	}

	if (gator_hrtimer_init(sample_rate, gator_timer_interrupt) == -1)
		return -1;

	for_each_online_cpu(cpu) {
		gator_timer_online_dispatch(lcpu_to_pcpu(cpu), false);
	}
	on_each_cpu(gator_timer_online, NULL, 1);

	return 0;
}

static u64 gator_get_time(void)
{
	struct timespec ts;
	u64 timestamp;
	u64 prev_timestamp;
	u64 delta;
	int cpu = smp_processor_id();

	// Match clock_gettime(CLOCK_MONOTONIC_RAW, &ts) from userspace
	getrawmonotonic(&ts);
	timestamp = timespec_to_ns(&ts);

	// getrawmonotonic is not monotonic on all systems. Detect and attempt to correct these cases.
	// up to 0.5ms delta has been seen on some systems, which can skew Streamline data when viewing at high resolution.
	prev_timestamp = per_cpu(last_timestamp, cpu);
	if (prev_timestamp <= timestamp) {
		per_cpu(last_timestamp, cpu) = timestamp;
	} else {
		delta = prev_timestamp - timestamp;
		// Log the error once
		if (!printed_monotonic_warning && delta > 500000) {
			printk(KERN_ERR "%s: getrawmonotonic is not monotonic  cpu: %i  delta: %lli\nSkew in Streamline data may be present at the fine zoom levels\n", __FUNCTION__, cpu, delta);
			printed_monotonic_warning = true;
		}
		timestamp = prev_timestamp;
	}

	return timestamp - gator_monotonic_started;
}

/******************************************************************************
 * cpu hotplug and pm notifiers
 ******************************************************************************/
#include "gator_iks.c"

static int __cpuinit gator_hotcpu_notify(struct notifier_block *self, unsigned long action, void *hcpu)
{
	int cpu = lcpu_to_pcpu((long)hcpu);

	switch (action) {
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		smp_call_function_single(cpu, gator_timer_offline, NULL, 1);
		gator_timer_offline_dispatch(cpu, false);
		break;
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		gator_timer_online_dispatch(cpu, false);
		smp_call_function_single(cpu, gator_timer_online, NULL, 1);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block __refdata gator_hotcpu_notifier = {
	.notifier_call = gator_hotcpu_notify,
};

// n.b. calling "on_each_cpu" only runs on those that are online
// Registered linux events are not disabled, so their counters will continue to collect
static int gator_pm_notify(struct notifier_block *nb, unsigned long event, void *dummy)
{
	int cpu;

	switch (event) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		unregister_hotcpu_notifier(&gator_hotcpu_notifier);
		unregister_scheduler_tracepoints();
		on_each_cpu(gator_timer_offline, NULL, 1);
		for_each_online_cpu(cpu) {
			gator_timer_offline_dispatch(lcpu_to_pcpu(cpu), false);
		}
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		for_each_online_cpu(cpu) {
			gator_timer_online_dispatch(lcpu_to_pcpu(cpu), false);
		}
		on_each_cpu(gator_timer_online, NULL, 1);
		register_scheduler_tracepoints();
		register_hotcpu_notifier(&gator_hotcpu_notifier);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block gator_pm_notifier = {
	.notifier_call = gator_pm_notify,
};

static int gator_notifier_start(void)
{
	int retval;
	retval = register_hotcpu_notifier(&gator_hotcpu_notifier);
	if (retval == 0)
		retval = register_pm_notifier(&gator_pm_notifier);
	return retval;
}

static void gator_notifier_stop(void)
{
	unregister_pm_notifier(&gator_pm_notifier);
	unregister_hotcpu_notifier(&gator_hotcpu_notifier);
}

/******************************************************************************
 * Main
 ******************************************************************************/
static void gator_summary(void)
{
	u64 timestamp, uptime;
	struct timespec ts;
	char uname_buf[512];
	void (*m2b)(struct timespec *ts);
	unsigned long flags;

	snprintf(uname_buf, sizeof(uname_buf), "%s %s %s %s %s GNU/Linux", utsname()->sysname, utsname()->nodename, utsname()->release, utsname()->version, utsname()->machine);

	getnstimeofday(&ts);
	timestamp = timespec_to_ns(&ts);

	do_posix_clock_monotonic_gettime(&ts);
	// monotonic_to_bootbased is not defined for some versions of Android
	m2b = symbol_get(monotonic_to_bootbased);
	if (m2b) {
		m2b(&ts);
	}
	uptime = timespec_to_ns(&ts);

	// Disable interrupts as gator_get_time calls smp_processor_id to verify time is monotonic
	local_irq_save(flags);
	// Set monotonic_started to zero as gator_get_time is uptime minus monotonic_started
	gator_monotonic_started = 0;
	gator_monotonic_started = gator_get_time();
	local_irq_restore(flags);

	marshal_summary(timestamp, uptime, uname_buf);
}

int gator_events_install(struct gator_interface *interface)
{
	list_add_tail(&interface->list, &gator_events);

	return 0;
}

int gator_events_get_key(void)
{
	// key of zero is reserved as a timestamp
	static int key = 1;

	const int ret = key;
	key += 2;
	return ret;
}

static int gator_init(void)
{
	int i;

	calc_first_cluster_size();

	// events sources (gator_events.h, generated by gator_events.sh)
	for (i = 0; i < ARRAY_SIZE(gator_events_list); i++)
		if (gator_events_list[i])
			gator_events_list[i]();

	gator_trace_sched_init();
	gator_trace_power_init();

	return 0;
}

static void gator_exit(void)
{
	struct gator_interface *gi;

	list_for_each_entry(gi, &gator_events, list)
		if (gi->shutdown)
			gi->shutdown();
}

static int gator_start(void)
{
	unsigned long cpu, i;
	struct gator_interface *gi;

	if (gator_migrate_start())
		goto migrate_failure;

	// Initialize the buffer with the frame type and core
	for_each_present_cpu(cpu) {
		for (i = 0; i < NUM_GATOR_BUFS; i++) {
			marshal_frame(cpu, i);
		}
		per_cpu(last_timestamp, cpu) = 0;
	}
	printed_monotonic_warning = false;

	// Capture the start time
	gator_summary();

	// start all events
	list_for_each_entry(gi, &gator_events, list) {
		if (gi->start && gi->start() != 0) {
			struct list_head *ptr = gi->list.prev;

			while (ptr != &gator_events) {
				gi = list_entry(ptr, struct gator_interface, list);

				if (gi->stop)
					gi->stop();

				ptr = ptr->prev;
			}
			goto events_failure;
		}
	}

	// cookies shall be initialized before trace_sched_start() and gator_timer_start()
	if (cookies_initialize())
		goto cookies_failure;
	if (gator_annotate_start())
		goto annotate_failure;
	if (gator_trace_sched_start())
		goto sched_failure;
	if (gator_trace_power_start())
		goto power_failure;
	if (gator_trace_gpu_start())
		goto gpu_failure;
	if (gator_timer_start(gator_timer_count))
		goto timer_failure;
	if (gator_notifier_start())
		goto notifier_failure;

	return 0;

notifier_failure:
	gator_timer_stop();
timer_failure:
	gator_trace_gpu_stop();
gpu_failure:
	gator_trace_power_stop();
power_failure:
	gator_trace_sched_stop();
sched_failure:
	gator_annotate_stop();
annotate_failure:
	cookies_release();
cookies_failure:
	// stop all events
	list_for_each_entry(gi, &gator_events, list)
		if (gi->stop)
			gi->stop();
events_failure:
	gator_migrate_stop();
migrate_failure:

	return -1;
}

static void gator_stop(void)
{
	struct gator_interface *gi;

	gator_annotate_stop();
	gator_trace_sched_stop();
	gator_trace_power_stop();
	gator_trace_gpu_stop();

	// stop all interrupt callback reads before tearing down other interfaces
	gator_notifier_stop();	// should be called before gator_timer_stop to avoid re-enabling the hrtimer after it has been offlined
	gator_timer_stop();

	// stop all events
	list_for_each_entry(gi, &gator_events, list)
		if (gi->stop)
			gi->stop();

	gator_migrate_stop();
}

/******************************************************************************
 * Filesystem
 ******************************************************************************/
/* fopen("buffer") */
static int gator_op_setup(void)
{
	int err = 0;
	int cpu, i;

	mutex_lock(&start_mutex);

	gator_buffer_size[SUMMARY_BUF] = SUMMARY_BUFFER_SIZE;
	gator_buffer_mask[SUMMARY_BUF] = SUMMARY_BUFFER_SIZE - 1;

	gator_buffer_size[BACKTRACE_BUF] = BACKTRACE_BUFFER_SIZE;
	gator_buffer_mask[BACKTRACE_BUF] = BACKTRACE_BUFFER_SIZE - 1;

	gator_buffer_size[NAME_BUF] = NAME_BUFFER_SIZE;
	gator_buffer_mask[NAME_BUF] = NAME_BUFFER_SIZE - 1;

	gator_buffer_size[COUNTER_BUF] = COUNTER_BUFFER_SIZE;
	gator_buffer_mask[COUNTER_BUF] = COUNTER_BUFFER_SIZE - 1;

	gator_buffer_size[BLOCK_COUNTER_BUF] = BLOCK_COUNTER_BUFFER_SIZE;
	gator_buffer_mask[BLOCK_COUNTER_BUF] = BLOCK_COUNTER_BUFFER_SIZE - 1;

	gator_buffer_size[ANNOTATE_BUF] = ANNOTATE_BUFFER_SIZE;
	gator_buffer_mask[ANNOTATE_BUF] = ANNOTATE_BUFFER_SIZE - 1;

	gator_buffer_size[SCHED_TRACE_BUF] = SCHED_TRACE_BUFFER_SIZE;
	gator_buffer_mask[SCHED_TRACE_BUF] = SCHED_TRACE_BUFFER_SIZE - 1;

	gator_buffer_size[GPU_TRACE_BUF] = GPU_TRACE_BUFFER_SIZE;
	gator_buffer_mask[GPU_TRACE_BUF] = GPU_TRACE_BUFFER_SIZE - 1;

	gator_buffer_size[IDLE_BUF] = IDLE_BUFFER_SIZE;
	gator_buffer_mask[IDLE_BUF] = IDLE_BUFFER_SIZE - 1;

	// Initialize percpu per buffer variables
	for (i = 0; i < NUM_GATOR_BUFS; i++) {
		// Verify buffers are a power of 2
		if (gator_buffer_size[i] & (gator_buffer_size[i] - 1)) {
			err = -ENOEXEC;
			goto setup_error;
		}

		for_each_present_cpu(cpu) {
			per_cpu(gator_buffer_read, cpu)[i] = 0;
			per_cpu(gator_buffer_write, cpu)[i] = 0;
			per_cpu(gator_buffer_commit, cpu)[i] = 0;
			per_cpu(buffer_space_available, cpu)[i] = true;
#if GATOR_LIVE
			per_cpu(gator_buffer_commit_time, cpu) = gator_live_rate;
#endif

			// Annotation is a special case that only uses a single buffer
			if (cpu > 0 && i == ANNOTATE_BUF) {
				per_cpu(gator_buffer, cpu)[i] = NULL;
				continue;
			}

			per_cpu(gator_buffer, cpu)[i] = vmalloc(gator_buffer_size[i]);
			if (!per_cpu(gator_buffer, cpu)[i]) {
				err = -ENOMEM;
				goto setup_error;
			}
		}
	}

setup_error:
	mutex_unlock(&start_mutex);
	return err;
}

/* Actually start profiling (echo 1>/dev/gator/enable) */
static int gator_op_start(void)
{
	int err = 0;

	mutex_lock(&start_mutex);

	if (gator_started || gator_start())
		err = -EINVAL;
	else
		gator_started = 1;

	mutex_unlock(&start_mutex);

	return err;
}

/* echo 0>/dev/gator/enable */
static void gator_op_stop(void)
{
	mutex_lock(&start_mutex);

	if (gator_started) {
		gator_stop();

		mutex_lock(&gator_buffer_mutex);

		gator_started = 0;
		gator_monotonic_started = 0;
		cookies_release();
		wake_up(&gator_buffer_wait);

		mutex_unlock(&gator_buffer_mutex);
	}

	mutex_unlock(&start_mutex);
}

static void gator_shutdown(void)
{
	int cpu, i;

	mutex_lock(&start_mutex);

	for_each_present_cpu(cpu) {
		mutex_lock(&gator_buffer_mutex);
		for (i = 0; i < NUM_GATOR_BUFS; i++) {
			vfree(per_cpu(gator_buffer, cpu)[i]);
			per_cpu(gator_buffer, cpu)[i] = NULL;
			per_cpu(gator_buffer_read, cpu)[i] = 0;
			per_cpu(gator_buffer_write, cpu)[i] = 0;
			per_cpu(gator_buffer_commit, cpu)[i] = 0;
			per_cpu(buffer_space_available, cpu)[i] = true;
#if GATOR_LIVE
			per_cpu(gator_buffer_commit_time, cpu) = 0;
#endif
		}
		mutex_unlock(&gator_buffer_mutex);
	}

	memset(&sent_core_name, 0, sizeof(sent_core_name));

	mutex_unlock(&start_mutex);
}

static int gator_set_backtrace(unsigned long val)
{
	int err = 0;

	mutex_lock(&start_mutex);

	if (gator_started)
		err = -EBUSY;
	else
		gator_backtrace_depth = val;

	mutex_unlock(&start_mutex);

	return err;
}

static ssize_t enable_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return gatorfs_ulong_to_user(gator_started, buf, count, offset);
}

static ssize_t enable_write(struct file *file, char const __user *buf, size_t count, loff_t *offset)
{
	unsigned long val;
	int retval;

	if (*offset)
		return -EINVAL;

	retval = gatorfs_ulong_from_user(&val, buf, count);
	if (retval)
		return retval;

	if (val)
		retval = gator_op_start();
	else
		gator_op_stop();

	if (retval)
		return retval;
	return count;
}

static const struct file_operations enable_fops = {
	.read = enable_read,
	.write = enable_write,
};

static int userspace_buffer_open(struct inode *inode, struct file *file)
{
	int err = -EPERM;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (test_and_set_bit_lock(0, &gator_buffer_opened))
		return -EBUSY;

	if ((err = gator_op_setup()))
		goto fail;

	/* NB: the actual start happens from userspace
	 * echo 1 >/dev/gator/enable
	 */

	return 0;

fail:
	__clear_bit_unlock(0, &gator_buffer_opened);
	return err;
}

static int userspace_buffer_release(struct inode *inode, struct file *file)
{
	gator_op_stop();
	gator_shutdown();
	__clear_bit_unlock(0, &gator_buffer_opened);
	return 0;
}

static ssize_t userspace_buffer_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	int commit, length1, length2, read;
	char *buffer1;
	char *buffer2;
	int cpu, buftype;
	int written = 0;

	// ensure there is enough space for a whole frame
	if (count < userspace_buffer_size || *offset) {
		return -EINVAL;
	}

	// sleep until the condition is true or a signal is received
	// the condition is checked each time gator_buffer_wait is woken up
	wait_event_interruptible(gator_buffer_wait, buffer_commit_ready(&cpu, &buftype) || !gator_started);

	if (signal_pending(current)) {
		return -EINTR;
	}

	if (buftype == -1 || cpu == -1) {
		return 0;
	}

	mutex_lock(&gator_buffer_mutex);

	do {
		read = per_cpu(gator_buffer_read, cpu)[buftype];
		commit = per_cpu(gator_buffer_commit, cpu)[buftype];

		// May happen if the buffer is freed during pending reads.
		if (!per_cpu(gator_buffer, cpu)[buftype]) {
			break;
		}

		// determine the size of two halves
		length1 = commit - read;
		length2 = 0;
		buffer1 = &(per_cpu(gator_buffer, cpu)[buftype][read]);
		buffer2 = &(per_cpu(gator_buffer, cpu)[buftype][0]);
		if (length1 < 0) {
			length1 = gator_buffer_size[buftype] - read;
			length2 = commit;
		}

		if (length1 + length2 > count - written) {
			break;
		}

		// start, middle or end
		if (length1 > 0 && copy_to_user(&buf[written], buffer1, length1)) {
			break;
		}

		// possible wrap around
		if (length2 > 0 && copy_to_user(&buf[written + length1], buffer2, length2)) {
			break;
		}

		per_cpu(gator_buffer_read, cpu)[buftype] = commit;
		written += length1 + length2;

		// Wake up annotate_write if more space is available
		if (buftype == ANNOTATE_BUF) {
			wake_up(&gator_annotate_wait);
		}
	}	while (buffer_commit_ready(&cpu, &buftype));

	mutex_unlock(&gator_buffer_mutex);

	// kick just in case we've lost an SMP event
	wake_up(&gator_buffer_wait);

	return written > 0 ? written : -EFAULT;
}

const struct file_operations gator_event_buffer_fops = {
	.open = userspace_buffer_open,
	.release = userspace_buffer_release,
	.read = userspace_buffer_read,
};

static ssize_t depth_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return gatorfs_ulong_to_user(gator_backtrace_depth, buf, count, offset);
}

static ssize_t depth_write(struct file *file, char const __user *buf, size_t count, loff_t *offset)
{
	unsigned long val;
	int retval;

	if (*offset)
		return -EINVAL;

	retval = gatorfs_ulong_from_user(&val, buf, count);
	if (retval)
		return retval;

	retval = gator_set_backtrace(val);

	if (retval)
		return retval;
	return count;
}

static const struct file_operations depth_fops = {
	.read = depth_read,
	.write = depth_write
};

void gator_op_create_files(struct super_block *sb, struct dentry *root)
{
	struct dentry *dir;
	struct gator_interface *gi;
	int cpu;

	/* reinitialize default values */
	gator_cpu_cores = 0;
	for_each_present_cpu(cpu) {
		gator_cpu_cores++;
	}
	userspace_buffer_size = BACKTRACE_BUFFER_SIZE;
	gator_response_type = 1;
	gator_live_rate = 0;

	gatorfs_create_file(sb, root, "enable", &enable_fops);
	gatorfs_create_file(sb, root, "buffer", &gator_event_buffer_fops);
	gatorfs_create_file(sb, root, "backtrace_depth", &depth_fops);
	gatorfs_create_ro_ulong(sb, root, "cpu_cores", &gator_cpu_cores);
	gatorfs_create_ro_ulong(sb, root, "buffer_size", &userspace_buffer_size);
	gatorfs_create_ulong(sb, root, "tick", &gator_timer_count);
	gatorfs_create_ulong(sb, root, "response_type", &gator_response_type);
	gatorfs_create_ro_ulong(sb, root, "version", &gator_protocol_version);
	gatorfs_create_ro_u64(sb, root, "started", &gator_monotonic_started);
	gatorfs_create_u64(sb, root, "live_rate", &gator_live_rate);

	// Annotate interface
	gator_annotate_create_files(sb, root);

	// Linux Events
	dir = gatorfs_mkdir(sb, root, "events");
	list_for_each_entry(gi, &gator_events, list)
		if (gi->create_files)
			gi->create_files(sb, dir);

	// Sched Events
	sched_trace_create_files(sb, dir);

	// Power interface
	gator_trace_power_create_files(sb, dir);
}

/******************************************************************************
 * Module
 ******************************************************************************/
static int __init gator_module_init(void)
{
	if (gatorfs_register()) {
		return -1;
	}

	if (gator_init()) {
		gatorfs_unregister();
		return -1;
	}

	setup_timer(&gator_buffer_wake_up_timer, gator_buffer_wake_up, 0);

	return 0;
}

static void __exit gator_module_exit(void)
{
	del_timer_sync(&gator_buffer_wake_up_timer);
	tracepoint_synchronize_unregister();
	gator_exit();
	gatorfs_unregister();
}

module_init(gator_module_init);
module_exit(gator_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM Ltd");
MODULE_DESCRIPTION("Gator system profiler");
