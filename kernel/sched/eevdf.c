// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone Earliest Eligible Virtual Deadline First scheduling class.
 *
 * Virtual-time accounting, lag-preserving placement and the augmented tree
 * search are adapted from Peter Zijlstra's Linux v6.6 fair scheduler.
 * This flat, experimental class deliberately leaves CFS intact. It supports
 * affinity-driven migration, but not automatic balancing, group scheduling
 * or PELT. See Documentation/scheduler/sched-eevdf.rst.
 */
#include <linux/rbtree_augmented.h>
#include <linux/sched/cputime.h>
#include <linux/sched/posix-timers.h>

#include "sched.h"
#include "stats.h"

#define EEVDF_WEIGHT_SCALE 1024
#define node_to_eevdf(node) rb_entry(node, struct sched_eevdf_entity, run_node)

static struct task_struct *eevdf_task(struct sched_eevdf_entity *se)
{
	return container_of(se, struct task_struct, eevdf);
}

static bool vtime_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

static u64 virtual_delta(u64 delta, unsigned long weight)
{
	return mul_u64_u32_div(delta, EEVDF_WEIGHT_SCALE, weight);
}

static s64 entity_key(struct eevdf_rq *erq, struct sched_eevdf_entity *se)
{
	return (s64)(se->vruntime - erq->min_vruntime);
}

/* The tree excludes curr; include it when computing V or eligibility. */
static void weighted_vruntime(struct eevdf_rq *erq, s64 *avg, u64 *load)
{
	struct sched_eevdf_entity *curr = erq->curr;

	*avg = erq->avg_vruntime;
	*load = erq->avg_load;
	if (curr && curr->on_rq) {
		*avg += entity_key(erq, curr) * curr->weight;
		*load += curr->weight;
	}
}

static u64 avg_vruntime(struct eevdf_rq *erq)
{
	s64 avg;
	u64 load;

	weighted_vruntime(erq, &avg, &load);
	if (load) {
		/* Round down so an entity placed at V is eligible. */
		if (avg < 0)
			avg -= load - 1;
		avg = div64_s64(avg, load);
	}
	return erq->min_vruntime + avg;
}

static bool entity_eligible(struct eevdf_rq *erq,
			    struct sched_eevdf_entity *se)
{
	s64 avg;
	u64 load;

	weighted_vruntime(erq, &avg, &load);
	return avg >= entity_key(erq, se) * (s64)load;
}

static void update_min_vruntime(struct eevdf_rq *erq)
{
	struct rb_node *left = rb_first_cached(&erq->tasks_timeline);
	struct sched_eevdf_entity *curr = erq->curr;
	u64 vruntime = erq->min_vruntime;
	s64 delta;

	if (curr && curr->on_rq)
		vruntime = curr->vruntime;
	else
		curr = NULL;
	if (left) {
		u64 first = node_to_eevdf(left)->vruntime;

		if (!curr || vtime_before(first, vruntime))
			vruntime = first;
	}
	delta = (s64)(vruntime - erq->min_vruntime);
	if (delta > 0) {
		erq->avg_vruntime -= delta * erq->avg_load;
		erq->min_vruntime = vruntime;
	}
}

static bool min_deadline_update(struct sched_eevdf_entity *se, bool exit)
{
	struct rb_node *node = &se->run_node;
	u64 old = se->min_deadline;
	struct sched_eevdf_entity *child;

	se->min_deadline = se->deadline;
	if (node->rb_left) {
		child = node_to_eevdf(node->rb_left);
		if (vtime_before(child->min_deadline, se->min_deadline))
			se->min_deadline = child->min_deadline;
	}
	if (node->rb_right) {
		child = node_to_eevdf(node->rb_right);
		if (vtime_before(child->min_deadline, se->min_deadline))
			se->min_deadline = child->min_deadline;
	}
	return old == se->min_deadline;
}

RB_DECLARE_CALLBACKS(static, min_deadline_cb, struct sched_eevdf_entity,
		     run_node, min_deadline, min_deadline_update);

static bool entity_less(struct rb_node *a, const struct rb_node *b)
{
	return vtime_before(node_to_eevdf(a)->vruntime,
			    node_to_eevdf(b)->vruntime);
}

static void insert_entity(struct eevdf_rq *erq, struct sched_eevdf_entity *se)
{
	struct rb_node **link = &erq->tasks_timeline.rb_root.rb_node;
	struct rb_node *parent = NULL;
	bool leftmost = true;

	erq->avg_vruntime += entity_key(erq, se) * se->weight;
	erq->avg_load += se->weight;
	se->min_deadline = se->deadline;
	while (*link) {
		struct sched_eevdf_entity *entry = node_to_eevdf(*link);

		parent = *link;
		if (vtime_before(se->deadline, entry->min_deadline))
			entry->min_deadline = se->deadline;
		if (entity_less(&se->run_node, parent))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&se->run_node, parent, link);
	rb_insert_augmented_cached(&se->run_node, &erq->tasks_timeline,
				   leftmost, &min_deadline_cb);
}

static void remove_entity(struct eevdf_rq *erq, struct sched_eevdf_entity *se)
{
	rb_erase_augmented_cached(&se->run_node, &erq->tasks_timeline,
				  &min_deadline_cb);
	RB_CLEAR_NODE(&se->run_node);
	erq->avg_vruntime -= entity_key(erq, se) * se->weight;
	erq->avg_load -= se->weight;
}

/* O(log n): vruntime orders eligibility, subtree minima order deadlines. */
static struct sched_eevdf_entity *pick_entity(struct eevdf_rq *erq)
{
	struct rb_node *node = erq->tasks_timeline.rb_root.rb_node;
	struct sched_eevdf_entity *best = erq->curr;
	struct sched_eevdf_entity *best_left = NULL;

	if (best && (!best->on_rq || !entity_eligible(erq, best)))
		best = NULL;

	while (node) {
		struct sched_eevdf_entity *se = node_to_eevdf(node);

		if (!entity_eligible(erq, se)) {
			node = node->rb_left;
			continue;
		}
		if (!best || vtime_before(se->deadline, best->deadline))
			best = se;
		if (node->rb_left) {
			struct sched_eevdf_entity *left = node_to_eevdf(node->rb_left);

			if (!best_left || vtime_before(left->min_deadline,
						     best_left->min_deadline))
				best_left = left;
			if (left->min_deadline == se->min_deadline)
				break;
		}
		if (se->deadline == se->min_deadline)
			break;
		node = node->rb_right;
	}
	if (!best_left || (best && !vtime_before(best_left->min_deadline,
						best->deadline)))
		return best;

	/* This entire subtree is eligible. Descend to its earliest deadline. */
	node = &best_left->run_node;
	while (node) {
		struct sched_eevdf_entity *se = node_to_eevdf(node);

		if (se->deadline == se->min_deadline)
			return se;
		if (node->rb_left &&
		    node_to_eevdf(node->rb_left)->min_deadline == se->min_deadline)
			node = node->rb_left;
		else
			node = node->rb_right;
	}
	return best;
}

static void update_curr_eevdf(struct rq *rq)
{
	struct eevdf_rq *erq = &rq->eevdf;
	struct sched_eevdf_entity *se = erq->curr;
	struct task_struct *p;
	u64 now, delta;

	if (!se || !se->on_rq)
		return;
	p = eevdf_task(se);
	now = rq_clock_task(rq);
	delta = now - p->se.exec_start;
	if ((s64)delta <= 0)
		return;

	schedstat_set(p->stats.exec_max, max(p->stats.exec_max, delta));
	update_current_exec_runtime(p, now, delta);
	se->vruntime += virtual_delta(delta, se->weight);
	trace_sched_stat_runtime(p, delta, se->vruntime);
	if (!vtime_before(se->vruntime, se->deadline)) {
		se->deadline = se->vruntime + virtual_delta(se->slice, se->weight);
		if (erq->nr_running > 1)
			resched_curr(rq);
	}
	update_min_vruntime(erq);
}

static void start_hrtick(struct rq *rq)
{
#ifdef CONFIG_SCHED_HRTICK
	struct sched_eevdf_entity *curr = rq->eevdf.curr;
	s64 remaining;
	u64 delay;

	/* Share HRTICK's feature switch with CFS for comparable experiments. */
	if (!hrtick_enabled_fair(rq) || !curr || rq->eevdf.nr_running < 2)
		return;
	remaining = curr->deadline - curr->vruntime;
	if (remaining <= 0) {
		resched_curr(rq);
		return;
	}
	delay = mul_u64_u32_div(remaining, curr->weight, EEVDF_WEIGHT_SCALE);
	hrtick_start(rq, delay);
#endif
}

static void place_entity(struct eevdf_rq *erq, struct sched_eevdf_entity *se,
			 int flags)
{
	u64 vruntime = avg_vruntime(erq);
	unsigned long weight = sched_prio_to_weight[task_nice(eevdf_task(se)) + 20];
	s64 remaining = se->deadline - se->vruntime;
	s64 lag = se->vlag;
	u64 load = erq->avg_load;

	/* Preserve service debt and the remaining request across reweighting. */
	if (se->weight && se->weight != weight) {
		lag = div64_s64(lag * se->weight, weight);
		remaining = div64_s64(remaining * se->weight, weight);
	}
	se->weight = weight;
	if (erq->curr && erq->curr->on_rq)
		load += erq->curr->weight;
	if (load)
		lag = div64_s64(lag * (s64)(load + weight), load);
	else
		lag = 0;
	se->vruntime = vruntime - lag;

	if (se->reset_deadline || (flags & ENQUEUE_WAKEUP) || remaining <= 0)
		remaining = virtual_delta(se->slice, weight);
	se->deadline = se->vruntime + remaining;
	se->reset_deadline = false;
}

static void enqueue_task_eevdf(struct rq *rq, struct task_struct *p, int flags)
{
	struct eevdf_rq *erq = &rq->eevdf;
	struct sched_eevdf_entity *se = &p->eevdf;

	update_curr_eevdf(rq);
	if (schedstat_enabled()) {
		if (!task_current(rq, p))
			__update_stats_wait_start(rq, p, &p->stats);
		if (flags & ENQUEUE_WAKEUP)
			__update_stats_enqueue_sleeper(rq, p, &p->stats);
	}
	place_entity(erq, se, flags);
	se->on_rq = 1;
	insert_entity(erq, se);
	erq->nr_running++;
	add_nr_running(rq, 1);
	/* Also cover slice/nice changes to an already runnable, remote task. */
	if (erq->curr && pick_entity(erq) != erq->curr)
		resched_curr(rq);
	start_hrtick(rq);
}

static void dequeue_task_eevdf(struct rq *rq, struct task_struct *p, int flags)
{
	struct eevdf_rq *erq = &rq->eevdf;
	struct sched_eevdf_entity *se = &p->eevdf;
	s64 limit;

	update_curr_eevdf(rq);
	if (schedstat_enabled()) {
		if (erq->curr != se && schedstat_val(p->stats.wait_start))
			__update_stats_wait_end(rq, p, &p->stats);
		if (flags & DEQUEUE_SLEEP) {
			unsigned int state = READ_ONCE(p->__state);

			if (state & TASK_INTERRUPTIBLE)
				__schedstat_set(p->stats.sleep_start, rq_clock(rq));
			if (state & TASK_UNINTERRUPTIBLE)
				__schedstat_set(p->stats.block_start, rq_clock(rq));
		}
	}
	limit = virtual_delta(max_t(u64, 2 * se->slice, TICK_NSEC), se->weight);
	se->vlag = clamp_t(s64, avg_vruntime(erq) - se->vruntime, -limit, limit);
	if (erq->curr == se)
		erq->curr = NULL;
	else
		remove_entity(erq, se);
	se->on_rq = 0;
	erq->nr_running--;
	sub_nr_running(rq, 1);
	update_min_vruntime(erq);
	start_hrtick(rq);
}

static void check_preempt_curr_eevdf(struct rq *rq, struct task_struct *p,
				    int flags)
{
	update_curr_eevdf(rq);
	if (pick_entity(&rq->eevdf) != rq->eevdf.curr)
		resched_curr(rq);
}

static void set_next_task_eevdf(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_eevdf_entity *se = &p->eevdf;

	if (schedstat_enabled() && schedstat_val(p->stats.wait_start))
		__update_stats_wait_end(rq, p, &p->stats);
	if (se->on_rq && !RB_EMPTY_NODE(&se->run_node))
		remove_entity(&rq->eevdf, se);
	rq->eevdf.curr = se;
	p->se.exec_start = rq_clock_task(rq);
	p->se.prev_sum_exec_runtime = p->se.sum_exec_runtime;
	start_hrtick(rq);
}

static void put_prev_task_eevdf(struct rq *rq, struct task_struct *p)
{
	struct sched_eevdf_entity *se = &p->eevdf;

	update_curr_eevdf(rq);
	if (se->on_rq) {
		if (schedstat_enabled())
			__update_stats_wait_start(rq, p, &p->stats);
		insert_entity(&rq->eevdf, se);
	}
	rq->eevdf.curr = NULL;
}

static struct task_struct *pick_task_eevdf(struct rq *rq)
{
	struct sched_eevdf_entity *se;

	if (!rq->eevdf.nr_running)
		return NULL;
	update_curr_eevdf(rq);
	se = pick_entity(&rq->eevdf);
	if (WARN_ON_ONCE(!se))
		return NULL;
	return eevdf_task(se);
}

static struct task_struct *pick_next_task_eevdf(struct rq *rq)
{
	struct task_struct *p = pick_task_eevdf(rq);

	if (p)
		set_next_task_eevdf(rq, p, true);
	return p;
}

static void task_tick_eevdf(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_eevdf(rq);
	if (queued || (rq->eevdf.nr_running > 1 &&
		       pick_entity(&rq->eevdf) != &p->eevdf))
		resched_curr(rq);
#ifdef CONFIG_SCHED_CORE
	/* A singleton must yield service to an incompatible SMT sibling too. */
	if (sched_core_enabled(rq) && rq->core->core_forceidle_count &&
	    p->se.sum_exec_runtime - p->se.prev_sum_exec_runtime >= p->eevdf.slice / 2)
		resched_curr(rq);
#endif
}

static void yield_task_eevdf(struct rq *rq)
{
	struct sched_eevdf_entity *se = &rq->curr->eevdf;

	update_curr_eevdf(rq);
	se->deadline += virtual_delta(se->slice, se->weight);
}

static void switched_to_eevdf(struct rq *rq, struct task_struct *p)
{
	if (!p->eevdf.on_rq)
		return;
	if (task_current(rq, p))
		resched_curr(rq);
	else
		check_preempt_curr(rq, p, 0);
}

static void prio_changed_eevdf(struct rq *rq, struct task_struct *p, int oldprio)
{
	switched_to_eevdf(rq, p);
}

static unsigned int get_rr_interval_eevdf(struct rq *rq, struct task_struct *p)
{
	return max_t(unsigned int, 1, nsecs_to_jiffies(p->eevdf.slice));
}

#ifdef CONFIG_SMP
static int balance_eevdf(struct rq *rq, struct task_struct *prev,
			 struct rq_flags *rf)
{
	return rq->eevdf.nr_running != 0;
}

static int select_task_rq_eevdf(struct task_struct *p, int cpu, int flags)
{
	/* Core validates this choice and supplies hotplug/affinity fallback. */
	return task_cpu(p);
}
#endif

#ifdef CONFIG_SCHED_CORE
void eevdf_vruntime_update(struct rq *rq, bool in_fi)
{
	struct eevdf_rq *erq = &rq->eevdf;

	if (in_fi) {
		if (erq->forceidle_seq == rq->core->core_forceidle_seq)
			return;
		erq->forceidle_seq = rq->core->core_forceidle_seq;
	}
	erq->min_vruntime_fi = erq->min_vruntime;
}

bool eevdf_prio_less(const struct task_struct *a, const struct task_struct *b,
		     bool in_fi)
{
	u64 da, db;

	/* Keep the origin fixed through forced idle so service cannot vanish. */
	eevdf_vruntime_update(task_rq(a), in_fi);
	eevdf_vruntime_update(task_rq(b), in_fi);
	da = a->eevdf.deadline - task_rq(a)->eevdf.min_vruntime_fi;
	db = b->eevdf.deadline - task_rq(b)->eevdf.min_vruntime_fi;

	return vtime_before(db, da);
}

static int task_is_throttled_eevdf(struct task_struct *p, int cpu)
{
	return !entity_eligible(&task_rq(p)->eevdf, &p->eevdf);
}
#endif

void init_eevdf_entity(struct task_struct *p)
{
	u64 slice = p->eevdf.slice;

	memset(&p->eevdf, 0, sizeof(p->eevdf));
	RB_CLEAR_NODE(&p->eevdf.run_node);
	p->eevdf.slice = slice ?: EEVDF_DEFAULT_SLICE;
	p->eevdf.reset_deadline = true;
}

void set_eevdf_slice(struct task_struct *p, u64 slice)
{
	slice = slice ?: EEVDF_DEFAULT_SLICE;
	if (p->eevdf.slice != slice) {
		p->eevdf.slice = slice;
		p->eevdf.reset_deadline = true;
	}
}

void init_eevdf_rq(struct eevdf_rq *erq)
{
	erq->tasks_timeline = RB_ROOT_CACHED;
	erq->min_vruntime = (u64)(-(1LL << 20));
#ifdef CONFIG_SCHED_CORE
	erq->min_vruntime_fi = erq->min_vruntime;
#endif
}

DEFINE_SCHED_CLASS(eevdf) = {
	.enqueue_task		= enqueue_task_eevdf,
	.dequeue_task		= dequeue_task_eevdf,
	.yield_task		= yield_task_eevdf,
	.check_preempt_curr	= check_preempt_curr_eevdf,
	.pick_next_task		= pick_next_task_eevdf,
	.put_prev_task		= put_prev_task_eevdf,
	.set_next_task		= set_next_task_eevdf,
	.task_tick		= task_tick_eevdf,
	.switched_to		= switched_to_eevdf,
	.prio_changed		= prio_changed_eevdf,
	.get_rr_interval	= get_rr_interval_eevdf,
	.update_curr		= update_curr_eevdf,
#ifdef CONFIG_SMP
	.balance		= balance_eevdf,
	.select_task_rq		= select_task_rq_eevdf,
	.pick_task		= pick_task_eevdf,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif
#ifdef CONFIG_SCHED_CORE
	.task_is_throttled	= task_is_throttled_eevdf,
#endif
};
