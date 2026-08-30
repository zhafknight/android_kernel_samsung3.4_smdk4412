#undef TRACE_SYSTEM
#define TRACE_SYSTEM oom

#if !defined(_TRACE_OOM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_OOM_H
#include <linux/tracepoint.h>

#define PG_COUNT_TO_KB(x) ((x) << (PAGE_SHIFT - 10))

TRACE_EVENT(oom_score_adj_update,

	TP_PROTO(struct task_struct *task),

	TP_ARGS(task),

	TP_STRUCT__entry(
		__field(	pid_t,	pid)
		__array(	char,	comm,	TASK_COMM_LEN )
		__field(	 int,	oom_score_adj)
	),

	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->oom_score_adj = task->signal->oom_score_adj;
	),

	TP_printk("pid=%d comm=%s oom_score_adj=%d",
		__entry->pid, __entry->comm, __entry->oom_score_adj)
);

TRACE_EVENT(mark_victim,

	TP_PROTO(struct task_struct *task, uid_t uid),

	TP_ARGS(task, uid),

	TP_STRUCT__entry(
		__field(	int,	pid		)
		__string(	comm,	task->comm	)
		__field(	u64,	total_vm	)
		__field(	u64,	anon_rss	)
		__field(	u64,	file_rss	)
		__field(	u64,	shmem_rss	)
		__field(	uid_t,	uid		)
		__field(	u64,	pgtables	)
		__field(	short,	oom_score_adj	)
	),

	TP_fast_assign(
		__entry->pid = task->pid;
		__assign_str(comm, task->comm);
		__entry->total_vm = PG_COUNT_TO_KB(task->mm->total_vm);
		__entry->anon_rss = PG_COUNT_TO_KB(
			get_mm_counter(task->mm, MM_ANONPAGES));
		__entry->file_rss = PG_COUNT_TO_KB(
			get_mm_counter(task->mm, MM_FILEPAGES));
		__entry->shmem_rss = 0;
		__entry->uid = uid;
		__entry->pgtables =
			(PTRS_PER_PTE * sizeof(pte_t) * task->mm->nr_ptes) >> 10;
		__entry->oom_score_adj = task->signal->oom_score_adj;
	),

	TP_printk("pid=%d comm=%s total-vm=%llukB anon-rss=%llukB file-rss=%llukB shmem-rss=%llukB uid=%u pgtables=%llukB oom_score_adj=%hd",
		__entry->pid, __get_str(comm), __entry->total_vm,
		__entry->anon_rss, __entry->file_rss, __entry->shmem_rss,
		__entry->uid, __entry->pgtables, __entry->oom_score_adj)
);

#endif

/* This part must be outside protection */
#include <trace/define_trace.h>
