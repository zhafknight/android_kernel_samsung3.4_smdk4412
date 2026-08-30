/*
 * Pressure stall information for CPU, memory and IO.
 *
 * Based on the upstream PSI implementation by Johannes Weiner and
 * the polling interface by Suren Baghdasaryan.  This version uses the
 * scheduler and workqueue interfaces available in the 3.4 kernel.
 */

#include <linux/ctype.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/psi.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define PSI_FREQ	(2 * HZ + 1)
#define EXP_10S		1677
#define EXP_60S		1981
#define EXP_300S	2034
#define WINDOW_MIN_US	500000
#define WINDOW_MAX_US	10000000
#define UPDATES_PER_WINDOW 10
#define PSI_LOAD_INT(x)	((x) >> FSHIFT)
#define PSI_LOAD_FRAC(x)	((((x) & (FIXED_1 - 1)) * 100) >> FSHIFT)

static bool psi_enabled = true;
static bool psi_initialized;
static DEFINE_PER_CPU(struct psi_group_cpu, system_group_pcpu);
static struct psi_group psi_system = {
	.pcpu = &system_group_pcpu,
};

static int __init setup_psi(char *str)
{
	if (!strcmp(str, "0"))
		psi_enabled = false;
	else if (!strcmp(str, "1"))
		psi_enabled = true;
	else
		return 0;
	return 1;
}
__setup("psi=", setup_psi);

static bool test_state(unsigned int *tasks, enum psi_states state)
{
	switch (state) {
	case PSI_IO_SOME:
		return tasks[NR_IOWAIT];
	case PSI_IO_FULL:
		return tasks[NR_IOWAIT] && !tasks[NR_RUNNING];
	case PSI_MEM_SOME:
		return tasks[NR_MEMSTALL];
	case PSI_MEM_FULL:
		return tasks[NR_MEMSTALL] && !tasks[NR_RUNNING];
	case PSI_CPU_SOME:
		return tasks[NR_RUNNING] > 1;
	case PSI_NONIDLE:
		return tasks[NR_IOWAIT] || tasks[NR_MEMSTALL] ||
			tasks[NR_RUNNING];
	default:
		return false;
	}
}

static void record_times(struct psi_group_cpu *groupc, int cpu,
			 bool memstall_tick)
{
	u64 now = cpu_clock(cpu);
	u32 delta = now - groupc->state_start;

	groupc->state_start = now;
	if (groupc->state_mask & (1 << PSI_IO_SOME)) {
		groupc->times[PSI_IO_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_IO_FULL))
			groupc->times[PSI_IO_FULL] += delta;
	}
	if (groupc->state_mask & (1 << PSI_MEM_SOME)) {
		groupc->times[PSI_MEM_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_MEM_FULL))
			groupc->times[PSI_MEM_FULL] += delta;
		else if (memstall_tick)
			groupc->times[PSI_MEM_FULL] +=
				min(delta, (u32)TICK_NSEC);
	}
	if (groupc->state_mask & (1 << PSI_CPU_SOME))
		groupc->times[PSI_CPU_SOME] += delta;
	if (groupc->state_mask & (1 << PSI_NONIDLE))
		groupc->times[PSI_NONIDLE] += delta;
}

static u32 psi_group_change(struct psi_group *group, int cpu,
			    unsigned int clear, unsigned int set)
{
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	unsigned int t;
	enum psi_states state;
	u32 state_mask = 0;

	write_seqcount_begin(&groupc->seq);
	record_times(groupc, cpu, false);
	for (t = 0; t < NR_PSI_TASK_COUNTS; t++) {
		if (clear & (1 << t)) {
			if (WARN_ON_ONCE(!groupc->tasks[t]))
				continue;
			groupc->tasks[t]--;
		}
		if (set & (1 << t))
			groupc->tasks[t]++;
	}
	for (state = 0; state < NR_PSI_STATES; state++) {
		if (test_state(groupc->tasks, state))
			state_mask |= 1 << state;
	}
	groupc->state_mask = state_mask;
	write_seqcount_end(&groupc->seq);
	return state_mask;
}

static bool get_recent_times(struct psi_group *group, int cpu, u32 *times)
{
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	u64 now, state_start;
	u32 state_mask;
	unsigned int seq;
	int state;

	do {
		seq = read_seqcount_begin(&groupc->seq);
		now = cpu_clock(cpu);
		memcpy(times, groupc->times, sizeof(groupc->times));
		state_mask = groupc->state_mask;
		state_start = groupc->state_start;
	} while (read_seqcount_retry(&groupc->seq, seq));

	for (state = 0; state < NR_PSI_STATES; state++) {
		if (state_mask & (1 << state))
			times[state] += now - state_start;
		times[state] -= groupc->times_prev[state];
		groupc->times_prev[state] += times[state];
	}
	return state_mask != 0;
}

static bool collect_times(struct psi_group *group)
{
	u64 deltas[NR_PSI_STATES - 1] = { 0 };
	unsigned long nonidle_total = 0;
	bool active = false;
	int cpu, state;

	for_each_possible_cpu(cpu) {
		u32 times[NR_PSI_STATES];
		unsigned long nonidle;

		active |= get_recent_times(group, cpu, times);
		nonidle = nsecs_to_jiffies(times[PSI_NONIDLE]);
		nonidle_total += nonidle;
		for (state = 0; state < PSI_NONIDLE; state++)
			deltas[state] += (u64)times[state] * nonidle;
	}
	for (state = 0; state < NR_PSI_STATES - 1; state++)
		group->total[state] += div64_u64(deltas[state],
						  max_t(unsigned long,
							nonidle_total, 1));
	return active;
}

static unsigned long calc_psi_load(unsigned long load, unsigned long exp,
				   unsigned long sample)
{
	return (load * exp + sample * (FIXED_1 - exp)) >> FSHIFT;
}

static void update_averages(struct psi_group *group, u64 now)
{
	u64 period = now - group->last_update;
	int state;

	if (!period)
		return;
	for (state = 0; state < NR_PSI_STATES - 1; state++) {
		u64 delta = group->total[state] - group->total_prev[state];
		unsigned long sample;

		if (delta > period)
			delta = period;
		sample = div64_u64(delta * 100 * FIXED_1, period);
		group->avg[state][0] = calc_psi_load(group->avg[state][0],
						      EXP_10S, sample);
		group->avg[state][1] = calc_psi_load(group->avg[state][1],
						      EXP_60S, sample);
		group->avg[state][2] = calc_psi_load(group->avg[state][2],
						      EXP_300S, sample);
		group->total_prev[state] = group->total[state];
	}
	group->last_update = now;
}

static void psi_clock_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psi_group *group = container_of(dwork, struct psi_group,
					       clock_work);

	bool active;

	mutex_lock(&group->lock);
	active = collect_times(group);
	update_averages(group, sched_clock());
	mutex_unlock(&group->lock);
	if (group->pcpu && active)
		schedule_delayed_work(&group->clock_work, PSI_FREQ);
}

static void psi_poll_work(struct work_struct *work);

static void group_init(struct psi_group *group)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);

		memset(groupc, 0, sizeof(*groupc));
		seqcount_init(&groupc->seq);
		groupc->state_start = cpu_clock(cpu);
	}
	mutex_init(&group->lock);
	mutex_init(&group->trigger_lock);
	INIT_LIST_HEAD(&group->triggers);
	INIT_DELAYED_WORK(&group->clock_work, psi_clock_work);
	INIT_DELAYED_WORK(&group->poll_work, psi_poll_work);
	group->last_update = sched_clock();
	group->poll_period = ULLONG_MAX;
	group->poll_states = 0;
	group->polling_until = 0;
}

void __init psi_init(void)
{
	if (!psi_enabled)
		return;
	group_init(&psi_system);
	psi_initialized = true;
}

static void psi_group_task_change(struct psi_group *group, int cpu,
				  unsigned int clear, unsigned int set)
{
	u32 state_mask;

	if (!group->pcpu)
		return;
	state_mask = psi_group_change(group, cpu, clear, set);
	if (!keventd_up())
		return;
	if (!delayed_work_pending(&group->clock_work))
		schedule_delayed_work(&group->clock_work, PSI_FREQ);
	if ((state_mask & ACCESS_ONCE(group->poll_states)) &&
	    !delayed_work_pending(&group->poll_work))
		schedule_delayed_work(&group->poll_work, 1);
}

void psi_task_change(struct task_struct *task, int clear, int set)
{
	int cpu;

	if (!psi_initialized || !task->pid)
		return;
	if (WARN_ON_ONCE((task->psi_flags & set) ||
			 (task->psi_flags & clear) != clear)) {
		clear &= task->psi_flags;
		set &= ~task->psi_flags;
	}
	if (!clear && !set)
		return;
	task->psi_flags &= ~clear;
	task->psi_flags |= set;
	cpu = task_cpu(task);
	psi_group_task_change(&psi_system, cpu, clear, set);
}

void psi_enqueue(struct task_struct *task, bool wakeup)
{
	int clear = 0, set = TSK_RUNNING;

	if (!psi_initialized || !task->pid)
		return;
	if (!wakeup || task->sched_psi_wake_requeue) {
		if (task->flags & PF_MEMSTALL)
			set |= TSK_MEMSTALL;
		task->sched_psi_wake_requeue = 0;
	} else if (task->in_iowait) {
		clear = TSK_IOWAIT;
	}
	psi_task_change(task, clear, set);
}

void psi_dequeue(struct task_struct *task, bool sleep)
{
	int clear = TSK_RUNNING, set = 0;

	if (!psi_initialized || !task->pid)
		return;
	if (!sleep) {
		if (task->flags & PF_MEMSTALL)
			clear |= TSK_MEMSTALL;
	} else if (task->in_iowait) {
		set = TSK_IOWAIT;
	}
	psi_task_change(task, clear, set);
}

void psi_memstall_tick(struct task_struct *task, int cpu)
{
	if (!psi_initialized || !(task->flags & PF_MEMSTALL))
		return;
	if (psi_system.pcpu) {
		struct psi_group_cpu *groupc = per_cpu_ptr(psi_system.pcpu, cpu);

		write_seqcount_begin(&groupc->seq);
		record_times(groupc, cpu, true);
		write_seqcount_end(&groupc->seq);
	}
}

void psi_memstall_enter(unsigned long *flags)
{
	unsigned long irqflags;

	if (!psi_initialized)
		return;
	*flags = current->flags & PF_MEMSTALL;
	if (*flags)
		return;
	preempt_disable();
	local_irq_save(irqflags);
	current->flags |= PF_MEMSTALL;
	psi_task_change(current, 0, TSK_MEMSTALL);
	local_irq_restore(irqflags);
	preempt_enable();
}

void psi_memstall_leave(unsigned long *flags)
{
	unsigned long irqflags;

	if (!psi_initialized || *flags)
		return;
	preempt_disable();
	local_irq_save(irqflags);
	current->flags &= ~PF_MEMSTALL;
	psi_task_change(current, TSK_MEMSTALL, 0);
	local_irq_restore(irqflags);
	preempt_enable();
}

static void window_reset(struct psi_window *window, u64 now, u64 value,
			 u64 previous)
{
	window->start_time = now;
	window->start_value = value;
	window->prev_growth = previous;
}

static u64 window_update(struct psi_window *window, u64 now, u64 value)
{
	u64 elapsed = now - window->start_time;
	u64 growth = value - window->start_value;

	if (elapsed > window->size)
		window_reset(window, now, value, growth);
	else
		growth += div64_u64(window->prev_growth *
					(window->size - elapsed), window->size);
	return growth;
}

static void psi_poll_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psi_group *group = container_of(dwork, struct psi_group,
					       poll_work);
	struct psi_trigger *trigger;
	bool activity = false;
	u64 now;

	mutex_lock(&group->lock);
	collect_times(group);
	now = sched_clock();
	mutex_unlock(&group->lock);

	mutex_lock(&group->trigger_lock);
	list_for_each_entry(trigger, &group->triggers, node) {
		u64 value = group->total[trigger->state];
		u64 growth;

		if (value != group->polling_total[trigger->state]) {
			activity = true;
			group->polling_until = max(group->polling_until,
						   now + trigger->win.size);
		}
		if (!trigger->win.start_time)
			window_reset(&trigger->win, now, value, 0);
		growth = window_update(&trigger->win, now, value);
		if (growth >= trigger->threshold &&
		    now >= trigger->last_event_time + trigger->win.size) {
			if (cmpxchg(&trigger->event, 0, 1) == 0)
				wake_up_interruptible(&trigger->event_wait);
			trigger->last_event_time = now;
		}
	}
	memcpy(group->polling_total, group->total,
	       sizeof(group->polling_total));
	if (!list_empty(&group->triggers) &&
	    (activity || now < group->polling_until))
		schedule_delayed_work(&group->poll_work,
			nsecs_to_jiffies(group->poll_period) + 1);
	mutex_unlock(&group->trigger_lock);
}

struct psi_trigger *psi_trigger_create(struct psi_group *group, char *buf,
				       size_t nbytes, enum psi_res res)
{
	struct psi_trigger *trigger;
	enum psi_states state;
	u32 threshold_us, window_us;

	if (!psi_initialized || !group->pcpu)
		return ERR_PTR(-EOPNOTSUPP);
	if (sscanf(buf, "some %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_SOME + res * 2;
	else if (sscanf(buf, "full %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_FULL + res * 2;
	else
		return ERR_PTR(-EINVAL);
	if (state >= PSI_NONIDLE || window_us < WINDOW_MIN_US ||
	    window_us > WINDOW_MAX_US || !threshold_us ||
	    threshold_us > window_us)
		return ERR_PTR(-EINVAL);

	trigger = kzalloc(sizeof(*trigger), GFP_KERNEL);
	if (!trigger)
		return ERR_PTR(-ENOMEM);
	trigger->group = group;
	trigger->state = state;
	trigger->threshold = (u64)threshold_us * NSEC_PER_USEC;
	trigger->win.size = (u64)window_us * NSEC_PER_USEC;
	init_waitqueue_head(&trigger->event_wait);
	INIT_LIST_HEAD(&trigger->node);

	mutex_lock(&group->lock);
	collect_times(group);
	mutex_unlock(&group->lock);
	mutex_lock(&group->trigger_lock);
	window_reset(&trigger->win, sched_clock(),
		     group->total[trigger->state], 0);
	list_add(&trigger->node, &group->triggers);
	group->poll_states |= 1 << trigger->state;
	memcpy(group->polling_total, group->total,
	       sizeof(group->polling_total));
	group->poll_period = min(group->poll_period,
		div64_u64(trigger->win.size, UPDATES_PER_WINDOW));
	if (!delayed_work_pending(&group->poll_work))
		schedule_delayed_work(&group->poll_work, 1);
	mutex_unlock(&group->trigger_lock);
	return trigger;
}

void psi_trigger_destroy(struct psi_trigger *trigger)
{
	struct psi_group *group;
	struct psi_trigger *iter;
	u64 period = ULLONG_MAX;
	u32 states = 0;
	bool empty;

	if (!trigger)
		return;
	wake_up_interruptible(&trigger->event_wait);
	group = trigger->group;
	mutex_lock(&group->trigger_lock);
	if (!list_empty(&trigger->node))
		list_del_init(&trigger->node);
	list_for_each_entry(iter, &group->triggers, node)
		states |= 1 << iter->state;
	list_for_each_entry(iter, &group->triggers, node)
		period = min(period,
			div64_u64(iter->win.size, UPDATES_PER_WINDOW));
	group->poll_period = period;
	group->poll_states = states;
	empty = list_empty(&group->triggers);
	mutex_unlock(&group->trigger_lock);
	if (empty)
		cancel_delayed_work_sync(&group->poll_work);
	kfree(trigger);
}

unsigned int psi_trigger_poll(void **trigger_ptr, struct file *file,
			      poll_table *wait)
{
	struct psi_trigger *trigger = *trigger_ptr;
	unsigned int mask = DEFAULT_POLLMASK;

	if (!trigger)
		return mask | POLLERR | POLLPRI;
	poll_wait(file, &trigger->event_wait, wait);
	if (cmpxchg(&trigger->event, 1, 0) == 1)
		mask |= POLLPRI;
	return mask;
}

int psi_show(struct seq_file *m, struct psi_group *group, enum psi_res res)
{
	int full, state, index;

	if (!psi_initialized || !group->pcpu)
		return -EOPNOTSUPP;
	mutex_lock(&group->lock);
	collect_times(group);
	update_averages(group, sched_clock());
	for (full = 0; full < 2 - (res == PSI_CPU); full++) {
		u64 total;

		state = res * 2 + full;
		total = div_u64(group->total[state], NSEC_PER_USEC);
		seq_printf(m, "%s", full ? "full" : "some");
		for (index = 0; index < 3; index++)
			seq_printf(m, " avg%d=%lu.%02lu",
				   index == 0 ? 10 : index == 1 ? 60 : 300,
				   PSI_LOAD_INT(group->avg[state][index]),
				   PSI_LOAD_FRAC(group->avg[state][index]));
		seq_printf(m, " total=%llu\n", total);
	}
	mutex_unlock(&group->lock);
	return 0;
}

static int psi_io_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_IO);
}

static int psi_memory_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_MEM);
}

static int psi_cpu_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_CPU);
}

static int psi_io_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_io_show, NULL);
}

static int psi_memory_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_memory_show, NULL);
}

static int psi_cpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_cpu_show, NULL);
}

static ssize_t psi_write(struct file *file, const char __user *user_buf,
			 size_t nbytes, enum psi_res res)
{
	struct seq_file *seq = file->private_data;
	struct psi_trigger *trigger;
	char buf[32];
	size_t size;

	if (!nbytes)
		return -EINVAL;
	size = min(nbytes, sizeof(buf) - 1);
	if (copy_from_user(buf, user_buf, size))
		return -EFAULT;
	buf[size] = '\0';
	mutex_lock(&seq->lock);
	if (seq->private) {
		mutex_unlock(&seq->lock);
		return -EBUSY;
	}
	trigger = psi_trigger_create(&psi_system, buf, nbytes, res);
	if (IS_ERR(trigger)) {
		mutex_unlock(&seq->lock);
		return PTR_ERR(trigger);
	}
	seq->private = trigger;
	mutex_unlock(&seq->lock);
	return nbytes;
}

static ssize_t psi_io_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	return psi_write(file, buf, count, PSI_IO);
}

static ssize_t psi_memory_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	return psi_write(file, buf, count, PSI_MEM);
}

static ssize_t psi_cpu_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	return psi_write(file, buf, count, PSI_CPU);
}

static unsigned int psi_fop_poll(struct file *file, poll_table *wait)
{
	struct seq_file *seq = file->private_data;

	return psi_trigger_poll(&seq->private, file, wait);
}

static int psi_fop_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	psi_trigger_destroy(seq->private);
	return single_release(inode, file);
}

#define PSI_FOPS(_name) { \
	.open = psi_##_name##_open, \
	.read = seq_read, \
	.write = psi_##_name##_write, \
	.poll = psi_fop_poll, \
	.llseek = seq_lseek, \
	.release = psi_fop_release, \
}

static const struct file_operations psi_io_fops = PSI_FOPS(io);
static const struct file_operations psi_memory_fops = PSI_FOPS(memory);
static const struct file_operations psi_cpu_fops = PSI_FOPS(cpu);

static int __init psi_proc_init(void)
{
	if (!psi_enabled)
		return 0;
	proc_mkdir("pressure", NULL);
	proc_create("pressure/io", 0, NULL, &psi_io_fops);
	proc_create("pressure/memory", 0, NULL, &psi_memory_fops);
	proc_create("pressure/cpu", 0, NULL, &psi_cpu_fops);
	return 0;
}
module_init(psi_proc_init);
