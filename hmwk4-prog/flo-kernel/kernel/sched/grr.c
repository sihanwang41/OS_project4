#include <linux/interrupt.h>

#include "sched.h"

/*
 * group round-robin scheduling class
 */

static inline struct task_struct *grr_task_of(struct sched_grr_entity *grr_se)
{
	return container_of(grr_se, struct task_struct, grr);
}

#ifdef CONFIG_SMP
static int task_grr_group(struct task_struct *p)
{
	char *s = task_group_path(task_group(p));

	if (strcmp(s, "/") == 0 || strcmp(s, "/apps") == 0)
		return FOREGROUND;

	return BACKGROUND;
}

static int
can_migrate_task(struct task_struct *p, struct grr_rq *src,
					struct grr_rq *dest)
{
	if (task_running(src->rq, p) ||
		task_grr_group(p) != dest->grr_group ||
		!cpumask_test_cpu(dest->rq->cpu, tsk_cpus_allowed(p)))
		return 0;

	return 1;
}

static void
migrate_task(struct task_struct *p, struct grr_rq *src, struct grr_rq *dest)
{
	deactivate_task(src->rq, p, 0);
	set_task_cpu(p, dest->rq->cpu);
	activate_task(dest->rq, p, 0);
}

static int
migrate_one_task(struct grr_rq *src, struct grr_rq *dest)
{
	struct sched_grr_entity *grr_se, *grr_se_temp;
	struct task_struct *p;

	list_for_each_entry_safe(grr_se, grr_se_temp,
				&src->run_list, run_list) {
		p = grr_task_of(grr_se);

		if (!can_migrate_task(p, src, dest))
			continue;

		migrate_task(p, src, dest);
		return 1;
	}

	return 0;
}

static int
select_task_rq_grr(struct task_struct *p, int sd_flag, int flags)
{
	int i, nr = 0;
	int cpu = task_cpu(p);
	int min_nr = -1;
	struct grr_rq *grr_rq;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;

	if (p->grr.nr_cpus_allowed == 1)
		goto select_task_rq_out;

	rcu_read_lock();
	for_each_cpu(i, rd->span) {
		grr_rq = &cpu_rq(i)->grr;
		nr = grr_rq->grr_nr_running;
		if ((task_grr_group(p) == grr_rq->grr_group) &&
				(cpumask_test_cpu(i, tsk_cpus_allowed(p))) &&
				(min_nr == -1 || nr < min_nr)) {
			min_nr = nr;
			cpu = i;
		}
	}
	rcu_read_unlock();

select_task_rq_out:
	return cpu;
}

static void select_task_rq_grr_and_migrate(struct task_struct *p)
{
	int cpu;
	struct grr_rq *src, *dest;
	unsigned long flags = 0;

	src = &cpu_rq(task_cpu(p))->grr;
	cpu = select_task_rq_grr(p, 0, flags);
	dest = &cpu_rq(cpu)->grr;

	local_irq_save(flags);
	double_rq_lock(src->rq, dest->rq);

	if (can_migrate_task(p, src, dest))
		migrate_task(p, src, dest);

	double_rq_unlock(src->rq, dest->rq);
	local_irq_restore(flags);
}

static void cgroup_attach_grr(struct task_struct *p)
{
	struct grr_rq *grr_rq = &cpu_rq(task_cpu(p))->grr;

	if (grr_rq->grr_group == task_grr_group(p))
		return;
	else if (p->on_rq)
		select_task_rq_grr_and_migrate(p);
}

static inline int on_null_domain(int cpu)
{
	return !rcu_dereference_sched(cpu_rq(cpu)->sd);
}

void trigger_load_balance_grr(struct rq *rq, int cpu)
{
	if (time_after_eq(jiffies, rq->next_balance_grr) &&
		likely(!on_null_domain(cpu)))
		raise_softirq(SCHED_GRR_SOFTIRQ);
}

static void run_rebalance_domains_grr(struct softirq_action *h)
{
	int i, min_nr, max_nr, nr;
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct root_domain *rd = rq->rd;
	struct grr_rq *grr_rq, *min_grr_rq, *max_grr_rq;
	int grr_group = rq->grr.grr_group;
	unsigned long flags = 0;

	rq->next_balance_grr = jiffies +
			msecs_to_jiffies(GRR_REBALANCE_INTERVAL);

	min_grr_rq = &rq->grr;
	min_nr = min_grr_rq->grr_nr_running;
	max_grr_rq = &rq->grr;
	max_nr = max_grr_rq->grr_nr_running;

	rcu_read_lock();
	for_each_cpu(i, rd->span) {
		grr_rq = &cpu_rq(i)->grr;
		nr = grr_rq->grr_nr_running;

		if (grr_rq->grr_group != grr_group)
			continue;

		if (nr < min_nr) {
			min_nr = nr;
			min_grr_rq = grr_rq;
		} else if (nr > max_nr) {
			max_nr = nr;
			max_grr_rq = grr_rq;
		}
	}
	rcu_read_unlock();

	if (min_grr_rq != max_grr_rq && max_nr > 1 && min_nr < max_nr - 1) {
		local_irq_save(flags);
		double_rq_lock(min_grr_rq->rq, max_grr_rq->rq);

		max_nr = max_grr_rq->grr_nr_running;
		min_nr = min_grr_rq->grr_nr_running;

		if (max_nr > 1 && min_nr < max_nr - 1)
			migrate_one_task(max_grr_rq, min_grr_rq);

		double_rq_unlock(min_grr_rq->rq, max_grr_rq->rq);
		local_irq_restore(flags);
	}
}

static inline int pick_grr_rq_group(int cpu, int n)
{
	return cpu < n ? FOREGROUND : BACKGROUND;
}

void change_grr_rq_groups(int n)
{
	int i, grr_group;
	struct sched_grr_entity *grr_se, *grr_se_temp;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;
	struct grr_rq *grr_rq;

	rcu_read_lock();
	for_each_cpu(i, rd->span) {
		grr_group = pick_grr_rq_group(i, n);
		grr_rq = &cpu_rq(i)->grr;

		if (grr_rq->grr_group != grr_group) {
			raw_spin_lock(&grr_rq->rq->lock);
			raw_spin_lock(&grr_rq->grr_runtime_lock);
			grr_rq->grr_group = grr_group;

			list_for_each_entry_safe(grr_se, grr_se_temp,
					&grr_rq->run_list, run_list)
				resched_task(grr_task_of(grr_se));

			raw_spin_unlock(&grr_rq->grr_runtime_lock);
			raw_spin_unlock(&grr_rq->rq->lock);
		}
	}
	rcu_read_unlock();
}

void set_grr_rq_group(struct grr_rq *grr_rq, int cpu)
{
	grr_rq->grr_group = pick_grr_rq_group(cpu, num_possible_cpus() / 2);
}

#endif /* CONFIG_SMP */

static void
check_preempt_curr_grr(struct rq *rq, struct task_struct *p, int flags)
{
}

void init_grr_rq(struct grr_rq *grr_rq, struct rq *rq)
{
	grr_rq->rq = rq;
	grr_rq->grr_nr_running = 0;
	INIT_LIST_HEAD(&grr_rq->run_list);
	raw_spin_lock_init(&grr_rq->grr_runtime_lock);
}

__init void init_sched_grr_class(void)
{
#ifdef CONFIG_SMP
	open_softirq(SCHED_GRR_SOFTIRQ, run_rebalance_domains_grr);
#endif
}

void idle_balance_grr(int this_cpu, struct rq *this_rq)
{
#ifdef CONFIG_SMP
	int i, res;
	struct grr_rq *grr_rq;
	unsigned long flags = 0;

	raw_spin_unlock(&this_rq->lock);

	rcu_read_lock();
	for_each_cpu(i, this_rq->rd->span) {
		grr_rq = &cpu_rq(i)->grr;
		if (i != this_cpu && grr_rq->grr_nr_running > 1) {
			local_irq_save(flags);
			double_rq_lock(grr_rq->rq, this_rq);

			if (grr_rq->grr_nr_running > 1)
				res = migrate_one_task(grr_rq, &this_rq->grr);

			double_rq_unlock(grr_rq->rq, this_rq);
			local_irq_restore(flags);

			if (res)
				break;
		}
	}
	rcu_read_unlock();

	raw_spin_lock(&this_rq->lock);
#endif
}

static struct task_struct *pick_next_task_grr(struct rq *rq)
{
	struct sched_grr_entity *grr_se;
	struct task_struct *p;
	struct grr_rq *grr_rq;

	grr_rq = &rq->grr;

	if (!grr_rq->grr_nr_running)
		return NULL;

	raw_spin_lock(&grr_rq->grr_runtime_lock);
	grr_se = list_first_entry(&grr_rq->run_list, struct sched_grr_entity,
							run_list);
	p = grr_task_of(grr_se);
	raw_spin_unlock(&grr_rq->grr_runtime_lock);

	return p;
}

static void
enqueue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_grr_entity *grr_se;
	struct grr_rq *grr_rq;

	grr_rq = &rq->grr;
	grr_se = &p->grr;

	raw_spin_lock(&grr_rq->grr_runtime_lock);
	list_add_tail(&grr_se->run_list, &grr_rq->run_list);
	grr_rq->grr_nr_running++;
	grr_se->on_rq = 1;
	raw_spin_unlock(&grr_rq->grr_runtime_lock);

	inc_nr_running(rq);
}

static void
dequeue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_grr_entity *grr_se;
	struct grr_rq *grr_rq;

	grr_rq = &rq->grr;
	grr_se = &p->grr;

	raw_spin_lock(&grr_rq->grr_runtime_lock);
	list_del(&grr_se->run_list);
	grr_rq->grr_nr_running--;
	grr_se->on_rq = 0;
	raw_spin_unlock(&grr_rq->grr_runtime_lock);

	dec_nr_running(rq);
}

static void yield_task_grr(struct rq *rq)
{
	struct sched_grr_entity *grr_se;
	struct grr_rq *grr_rq;

	grr_rq = &rq->grr;
	grr_se = &rq->curr->grr;

	raw_spin_lock(&grr_rq->grr_runtime_lock);
	list_move_tail(&grr_se->run_list, &grr_rq->run_list);
	raw_spin_unlock(&grr_rq->grr_runtime_lock);
}

static void put_prev_task_grr(struct rq *rq, struct task_struct *prev)
{
	struct sched_grr_entity *grr_se;
	struct grr_rq *grr_rq;

	grr_rq = &rq->grr;
	grr_se = &prev->grr;

	if (grr_se->on_rq) {
		raw_spin_lock(&grr_rq->grr_runtime_lock);
		list_move_tail(&grr_se->run_list, &grr_rq->run_list);
		raw_spin_unlock(&grr_rq->grr_runtime_lock);
	}
}

static void task_tick_grr(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_grr_entity *grr_se;
	struct grr_rq *grr_rq;

	grr_rq = &rq->grr;
	grr_se = &p->grr;

	if (--p->grr.time_slice)
		return;

	p->grr.time_slice = GRR_TIMESLICE;

	if (grr_se->run_list.prev != grr_se->run_list.next)
		resched_task(p);
}

static void set_curr_task_grr(struct rq *rq)
{
}

static void switched_to_grr(struct rq *rq, struct task_struct *p)
{
}

static void
prio_changed_grr(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static unsigned int get_rr_interval_grr(struct rq *rq, struct task_struct *task)
{
	return GRR_TIMESLICE;
}

/*
 * Group round-robin scheduling class
 */
const struct sched_class grr_sched_class = {
	.next			= &fair_sched_class,

	.enqueue_task		= enqueue_task_grr,
	.dequeue_task		= dequeue_task_grr,
	.yield_task		= yield_task_grr,

	.check_preempt_curr	= check_preempt_curr_grr,

	.pick_next_task		= pick_next_task_grr,
	.put_prev_task		= put_prev_task_grr,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_grr,
	.cgroup_attach		= cgroup_attach_grr,
#endif

	.set_curr_task          = set_curr_task_grr,
	.task_tick		= task_tick_grr,

	.get_rr_interval	= get_rr_interval_grr,

	.prio_changed		= prio_changed_grr,
	.switched_to		= switched_to_grr,
};
