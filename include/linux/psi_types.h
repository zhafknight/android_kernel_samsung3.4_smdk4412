#ifndef _LINUX_PSI_TYPES_H
#define _LINUX_PSI_TYPES_H

#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/seqlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#ifdef CONFIG_PSI

enum psi_task_count {
	NR_IOWAIT,
	NR_MEMSTALL,
	NR_RUNNING,
	NR_PSI_TASK_COUNTS,
};

#define TSK_IOWAIT	(1 << NR_IOWAIT)
#define TSK_MEMSTALL	(1 << NR_MEMSTALL)
#define TSK_RUNNING	(1 << NR_RUNNING)

enum psi_res {
	PSI_IO,
	PSI_MEM,
	PSI_CPU,
	NR_PSI_RESOURCES,
};

enum psi_states {
	PSI_IO_SOME,
	PSI_IO_FULL,
	PSI_MEM_SOME,
	PSI_MEM_FULL,
	PSI_CPU_SOME,
	PSI_NONIDLE,
	NR_PSI_STATES,
};

struct psi_group_cpu {
	seqcount_t seq;
	unsigned int tasks[NR_PSI_TASK_COUNTS];
	u32 state_mask;
	u32 times[NR_PSI_STATES];
	u32 times_prev[NR_PSI_STATES];
	u64 state_start;
};

struct psi_window {
	u64 size;
	u64 start_time;
	u64 start_value;
	u64 prev_growth;
};

struct psi_group;

struct psi_trigger {
	enum psi_states state;
	u64 threshold;
	struct list_head node;
	struct psi_group *group;
	wait_queue_head_t event_wait;
	int event;
	struct psi_window win;
	u64 last_event_time;
};

struct psi_group {
	struct mutex lock;
	struct psi_group_cpu __percpu *pcpu;
	u64 total[NR_PSI_STATES - 1];
	u64 total_prev[NR_PSI_STATES - 1];
	unsigned long avg[NR_PSI_STATES - 1][3];
	u64 last_update;
	struct delayed_work clock_work;
	struct mutex trigger_lock;
	struct list_head triggers;
	struct delayed_work poll_work;
	u64 poll_period;
	u32 poll_states;
	u64 polling_total[NR_PSI_STATES - 1];
	u64 polling_until;
};

#else

struct psi_group { };

#endif

#endif
