#ifndef _LINUX_PSI_H
#define _LINUX_PSI_H

#include <linux/psi_types.h>
#include <linux/poll.h>

struct seq_file;
struct task_struct;

#ifdef CONFIG_PSI

void psi_init(void);
void psi_enqueue(struct task_struct *task, bool wakeup);
void psi_dequeue(struct task_struct *task, bool sleep);
void psi_task_change(struct task_struct *task, int clear, int set);
void psi_memstall_tick(struct task_struct *task, int cpu);
void psi_memstall_enter(unsigned long *flags);
void psi_memstall_leave(unsigned long *flags);
int psi_show(struct seq_file *m, struct psi_group *group, enum psi_res res);
struct psi_trigger *psi_trigger_create(struct psi_group *group, char *buf,
				       size_t nbytes, enum psi_res res);
void psi_trigger_destroy(struct psi_trigger *trigger);
unsigned int psi_trigger_poll(void **trigger, struct file *file,
			      poll_table *wait);

#else

static inline void psi_init(void) { }
static inline void psi_enqueue(struct task_struct *task, bool wakeup) { }
static inline void psi_dequeue(struct task_struct *task, bool sleep) { }
static inline void psi_memstall_tick(struct task_struct *task, int cpu) { }
static inline void psi_memstall_enter(unsigned long *flags) { }
static inline void psi_memstall_leave(unsigned long *flags) { }

#endif

#endif
