// SPDX-License-Identifier: GPL-2.0
/*
 * Data Access Monitor
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 */

#define pr_fmt(fmt) "damon: " fmt

#include <linux/damon.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/slab.h>

#define CREATE_TRACE_POINTS
#include <trace/events/damon.h>

#ifdef CONFIG_DAMON_KUNIT_TEST
#undef DAMON_MIN_REGION
#define DAMON_MIN_REGION 1
#endif

/* Get a random number in [l, r) */
#define damon_rand(l, r) (l + prandom_u32_max(r - l))

static DEFINE_MUTEX(damon_lock);
static int nr_running_ctxs;

/*
 * Construct a damon_region struct
 *
 * Returns the pointer to the new struct if success, or NULL otherwise
 */
struct damon_region *damon_new_region(unsigned long start, unsigned long end)
{
	struct damon_region *region;

	region = kmalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	region->ar.start = start;
	region->ar.end = end;
	region->nr_accesses = 0;
	INIT_LIST_HEAD(&region->list);

	region->age = 0;
	region->last_nr_accesses = 0;

	return region;
}

/*
 * Add a region between two other regions
 */
inline void damon_insert_region(struct damon_region *r,
		struct damon_region *prev, struct damon_region *next)
{
	__list_add(&r->list, &prev->list, &next->list);
}

void damon_add_region(struct damon_region *r, struct damon_target *t)
{
	list_add_tail(&r->list, &t->regions_list);
}

static void damon_del_region(struct damon_region *r)
{
	list_del(&r->list);
}

static void damon_free_region(struct damon_region *r)
{
	kfree(r);
}

void damon_destroy_region(struct damon_region *r)
{
	damon_del_region(r);
	damon_free_region(r);
}

struct damos *damon_new_scheme(
		unsigned long min_sz_region, unsigned long max_sz_region,
		unsigned int min_nr_accesses, unsigned int max_nr_accesses,
		unsigned int min_age_region, unsigned int max_age_region,
		enum damos_action action)
{
	struct damos *scheme;

	scheme = kmalloc(sizeof(*scheme), GFP_KERNEL);
	if (!scheme)
		return NULL;
	scheme->min_sz_region = min_sz_region;
	scheme->max_sz_region = max_sz_region;
	scheme->min_nr_accesses = min_nr_accesses;
	scheme->max_nr_accesses = max_nr_accesses;
	scheme->min_age_region = min_age_region;
	scheme->max_age_region = max_age_region;
	scheme->action = action;
	scheme->stat_count = 0;
	scheme->stat_sz = 0;
	INIT_LIST_HEAD(&scheme->list);

	return scheme;
}

void damon_add_scheme(struct damon_ctx *ctx, struct damos *s)
{
	list_add_tail(&s->list, &ctx->schemes);
}

static void damon_del_scheme(struct damos *s)
{
	list_del(&s->list);
}

static void damon_free_scheme(struct damos *s)
{
	kfree(s);
}

void damon_destroy_scheme(struct damos *s)
{
	damon_del_scheme(s);
	damon_free_scheme(s);
}

/*
 * Construct a damon_target struct
 *
 * Returns the pointer to the new struct if success, or NULL otherwise
 */
struct damon_target *damon_new_target(unsigned long id)
{
	struct damon_target *t;

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;

	t->id = id;
	INIT_LIST_HEAD(&t->regions_list);

	return t;
}

void damon_add_target(struct damon_ctx *ctx, struct damon_target *t)
{
	list_add_tail(&t->list, &ctx->adaptive_targets);
}

static void damon_del_target(struct damon_target *t)
{
	list_del(&t->list);
}

void damon_free_target(struct damon_target *t)
{
	struct damon_region *r, *next;

	damon_for_each_region_safe(r, next, t)
		damon_free_region(r);
	kfree(t);
}

void damon_destroy_target(struct damon_target *t)
{
	damon_del_target(t);
	damon_free_target(t);
}

unsigned int damon_nr_regions(struct damon_target *t)
{
	struct damon_region *r;
	unsigned int nr_regions = 0;

	damon_for_each_region(r, t)
		nr_regions++;

	return nr_regions;
}

struct damon_ctx *damon_new_ctx(enum damon_target_type type)
{
	struct damon_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->sample_interval = 5 * 1000;
	ctx->aggr_interval = 100 * 1000;
	ctx->primitive_update_interval = 1000 * 1000;

	ktime_get_coarse_ts64(&ctx->last_aggregation);
	ctx->last_primitive_update = ctx->last_aggregation;

	mutex_init(&ctx->kdamond_lock);

	ctx->target_type = type;
	if (type != DAMON_ARBITRARY_TARGET) {
		ctx->min_nr_regions = 10;
		ctx->max_nr_regions = 1000;

		INIT_LIST_HEAD(&ctx->adaptive_targets);
		INIT_LIST_HEAD(&ctx->schemes);
	}

	return ctx;
}

static void damon_destroy_targets(struct damon_ctx *ctx)
{
	struct damon_target *t, *next_t;

	if (ctx->primitive.cleanup) {
		ctx->primitive.cleanup(ctx);
		return;
	}

	damon_for_each_target_safe(t, next_t, ctx)
		damon_destroy_target(t);
}

void damon_destroy_ctx(struct damon_ctx *ctx)
{
	struct damos *s, *next_s;

	damon_destroy_targets(ctx);

	damon_for_each_scheme_safe(s, next_s, ctx)
		damon_destroy_scheme(s);

	kfree(ctx);
}

/**
 * damon_set_targets() - Set monitoring targets.
 * @ctx:	monitoring context
 * @ids:	array of target ids
 * @nr_ids:	number of entries in @ids
 *
 * This function should not be called while the kdamond is running.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_set_targets(struct damon_ctx *ctx,
		      unsigned long *ids, ssize_t nr_ids)
{
	ssize_t i;
	struct damon_target *t, *next;

	damon_destroy_targets(ctx);

	for (i = 0; i < nr_ids; i++) {
		t = damon_new_target(ids[i]);
		if (!t) {
			pr_err("Failed to alloc damon_target\n");
			/* The caller should do cleanup of the ids itself */
			damon_for_each_target_safe(t, next, ctx)
				damon_destroy_target(t);
			return -ENOMEM;
		}
		damon_add_target(ctx, t);
	}

	return 0;
}

/**
 * damon_set_attrs() - Set attributes for the monitoring.
 * @ctx:		monitoring context
 * @sample_int:		time interval between samplings
 * @aggr_int:		time interval between aggregations
 * @primitive_upd_int:	time interval between monitoring primitive updates
 * @min_nr_reg:		minimal number of regions
 * @max_nr_reg:		maximum number of regions
 *
 * This function should not be called while the kdamond is running.
 * Every time interval is in micro-seconds.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_set_attrs(struct damon_ctx *ctx, unsigned long sample_int,
		    unsigned long aggr_int, unsigned long primitive_upd_int,
		    unsigned long min_nr_reg, unsigned long max_nr_reg)
{
	if (min_nr_reg < 3) {
		pr_err("min_nr_regions (%lu) must be at least 3\n",
				min_nr_reg);
		return -EINVAL;
	}
	if (min_nr_reg > max_nr_reg) {
		pr_err("invalid nr_regions.  min (%lu) > max (%lu)\n",
				min_nr_reg, max_nr_reg);
		return -EINVAL;
	}

	ctx->sample_interval = sample_int;
	ctx->aggr_interval = aggr_int;
	ctx->primitive_update_interval = primitive_upd_int;
	if (ctx->target_type != DAMON_ARBITRARY_TARGET) {
		ctx->min_nr_regions = min_nr_reg;
		ctx->max_nr_regions = max_nr_reg;
	}

	return 0;
}

/**
 * damon_set_schemes() - Set data access monitoring based operation schemes.
 * @ctx:	monitoring context
 * @schemes:	array of the schemes
 * @nr_schemes:	number of entries in @schemes
 *
 * This function should not be called while the kdamond of the context is
 * running.
 *
 * Return: 0 if success, or negative error code otherwise.
 */
int damon_set_schemes(struct damon_ctx *ctx, struct damos **schemes,
			ssize_t nr_schemes)
{
	struct damos *s, *next;
	ssize_t i;

	damon_for_each_scheme_safe(s, next, ctx)
		damon_destroy_scheme(s);
	for (i = 0; i < nr_schemes; i++)
		damon_add_scheme(ctx, schemes[i]);
	return 0;
}

/**
 * damon_nr_running_ctxs() - Return number of currently running contexts.
 */
int damon_nr_running_ctxs(void)
{
	int nr_ctxs;

	mutex_lock(&damon_lock);
	nr_ctxs = nr_running_ctxs;
	mutex_unlock(&damon_lock);

	return nr_ctxs;
}

/* Returns the size upper limit for each monitoring region */
static unsigned long damon_region_sz_limit(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned long sz = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			sz += r->ar.end - r->ar.start;
	}

	if (ctx->min_nr_regions)
		sz /= ctx->min_nr_regions;
	if (sz < DAMON_MIN_REGION)
		sz = DAMON_MIN_REGION;

	return sz;
}

static bool damon_kdamond_running(struct damon_ctx *ctx)
{
	bool running;

	mutex_lock(&ctx->kdamond_lock);
	running = ctx->kdamond != NULL;
	mutex_unlock(&ctx->kdamond_lock);

	return running;
}

static int kdamond_fn(void *data);

/*
 * __damon_start() - Starts monitoring with given context.
 * @ctx:	monitoring context
 *
 * This function should be called while damon_lock is hold.
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int __damon_start(struct damon_ctx *ctx)
{
	int err = -EBUSY;

	mutex_lock(&ctx->kdamond_lock);
	if (!ctx->kdamond) {
		err = 0;
		ctx->kdamond_stop = false;
		ctx->kdamond = kthread_create(kdamond_fn, ctx, "kdamond.%d",
				nr_running_ctxs);
		if (IS_ERR(ctx->kdamond))
			err = PTR_ERR(ctx->kdamond);
		else
			wake_up_process(ctx->kdamond);
	}
	mutex_unlock(&ctx->kdamond_lock);

	return err;
}

/**
 * damon_start() - Starts the monitorings for a given group of contexts.
 * @ctxs:	an array of the pointers for contexts to start monitoring
 * @nr_ctxs:	size of @ctxs
 *
 * This function starts a group of monitoring threads for a group of monitoring
 * contexts.  One thread per each context is created and run in parallel.  The
 * caller should handle synchronization between the threads by itself.  If a
 * group of threads that created by other 'damon_start()' call is currently
 * running, this function does nothing but returns -EBUSY.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_start(struct damon_ctx **ctxs, int nr_ctxs)
{
	int i;
	int err = 0;

	mutex_lock(&damon_lock);
	if (nr_running_ctxs) {
		mutex_unlock(&damon_lock);
		return -EBUSY;
	}

	for (i = 0; i < nr_ctxs; i++) {
		err = __damon_start(ctxs[i]);
		if (err)
			break;
		nr_running_ctxs++;
	}
	mutex_unlock(&damon_lock);

	return err;
}

/*
 * __damon_stop() - Stops monitoring of given context.
 * @ctx:	monitoring context
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int __damon_stop(struct damon_ctx *ctx)
{
	mutex_lock(&ctx->kdamond_lock);
	if (ctx->kdamond) {
		ctx->kdamond_stop = true;
		mutex_unlock(&ctx->kdamond_lock);
		while (damon_kdamond_running(ctx))
			usleep_range(ctx->sample_interval,
					ctx->sample_interval * 2);
		return 0;
	}
	mutex_unlock(&ctx->kdamond_lock);

	return -EPERM;
}

/**
 * damon_stop() - Stops the monitorings for a given group of contexts.
 * @ctxs:	an array of the pointers for contexts to stop monitoring
 * @nr_ctxs:	size of @ctxs
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_stop(struct damon_ctx **ctxs, int nr_ctxs)
{
	int i, err = 0;

	for (i = 0; i < nr_ctxs; i++) {
		/* nr_running_ctxs is decremented in kdamond_fn */
		err = __damon_stop(ctxs[i]);
		if (err)
			return err;
	}

	return err;
}

/*
 * damon_check_reset_time_interval() - Check if a time interval is elapsed.
 * @baseline:	the time to check whether the interval has elapsed since
 * @interval:	the time interval (microseconds)
 *
 * See whether the given time interval has passed since the given baseline
 * time.  If so, it also updates the baseline to current time for next check.
 *
 * Return:	true if the time interval has passed, or false otherwise.
 */
static bool damon_check_reset_time_interval(struct timespec64 *baseline,
		unsigned long interval)
{
	struct timespec64 now;

	ktime_get_coarse_ts64(&now);
	if ((timespec64_to_ns(&now) - timespec64_to_ns(baseline)) <
			interval * 1000)
		return false;
	*baseline = now;
	return true;
}

/*
 * Check whether it is time to flush the aggregated information
 */
static bool kdamond_aggregate_interval_passed(struct damon_ctx *ctx)
{
	return damon_check_reset_time_interval(&ctx->last_aggregation,
			ctx->aggr_interval);
}

/*
 * Reset the aggregated monitoring results ('nr_accesses' of each region).
 */
static void kdamond_reset_aggregated(struct damon_ctx *c)
{
	struct damon_target *t;

	damon_for_each_target(t, c) {
		struct damon_region *r;

		damon_for_each_region(r, t) {
			trace_damon_aggregated(t, r, damon_nr_regions(t));
			r->last_nr_accesses = r->nr_accesses;
			r->nr_accesses = 0;
		}
	}
}

static void damon_do_apply_schemes(struct damon_ctx *c,
				   struct damon_target *t,
				   struct damon_region *r)
{
	struct damos *s;
	unsigned long sz;

	damon_for_each_scheme(s, c) {
		sz = r->ar.end - r->ar.start;
		if (sz < s->min_sz_region || s->max_sz_region < sz)
			continue;
		if (r->nr_accesses < s->min_nr_accesses ||
				s->max_nr_accesses < r->nr_accesses)
			continue;
		if (r->age < s->min_age_region || s->max_age_region < r->age)
			continue;
		s->stat_count++;
		s->stat_sz += sz;
		if (c->primitive.apply_scheme)
			c->primitive.apply_scheme(c, t, r, s);
		if (s->action != DAMOS_STAT)
			r->age = 0;
	}
}

static void kdamond_apply_schemes(struct damon_ctx *c)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, c) {
		damon_for_each_region(r, t)
			damon_do_apply_schemes(c, t, r);
	}
}

#define sz_damon_region(r) (r->ar.end - r->ar.start)

/*
 * Merge two adjacent regions into one region
 */
static void damon_merge_two_regions(struct damon_region *l,
				struct damon_region *r)
{
	unsigned long sz_l = sz_damon_region(l), sz_r = sz_damon_region(r);

	l->nr_accesses = (l->nr_accesses * sz_l + r->nr_accesses * sz_r) /
			(sz_l + sz_r);
	l->age = (l->age * sz_l + r->age * sz_r) / (sz_l + sz_r);
	l->ar.end = r->ar.end;
	damon_destroy_region(r);
}

#define diff_of(a, b) (a > b ? a - b : b - a)

/*
 * Merge adjacent regions having similar access frequencies
 *
 * t		target affected by this merge operation
 * thres	'->nr_accesses' diff threshold for the merge
 * sz_limit	size upper limit of each region
 */
static void damon_merge_regions_of(struct damon_target *t, unsigned int thres,
				   unsigned long sz_limit)
{
	struct damon_region *r, *prev = NULL, *next;

	damon_for_each_region_safe(r, next, t) {
		if (diff_of(r->nr_accesses, r->last_nr_accesses) > thres)
			r->age = 0;
		else
			r->age++;

		if (prev && prev->ar.end == r->ar.start &&
		    diff_of(prev->nr_accesses, r->nr_accesses) <= thres &&
		    sz_damon_region(prev) + sz_damon_region(r) <= sz_limit)
			damon_merge_two_regions(prev, r);
		else
			prev = r;
	}
}

/*
 * Merge adjacent regions having similar access frequencies
 *
 * threshold	'->nr_accesses' diff threshold for the merge
 * sz_limit	size upper limit of each region
 *
 * This function merges monitoring target regions which are adjacent and their
 * access frequencies are similar.  This is for minimizing the monitoring
 * overhead under the dynamically changeable access pattern.  If a merge was
 * unnecessarily made, later 'kdamond_split_regions()' will revert it.
 */
static void kdamond_merge_regions(struct damon_ctx *c, unsigned int threshold,
				  unsigned long sz_limit)
{
	struct damon_target *t;

	damon_for_each_target(t, c)
		damon_merge_regions_of(t, threshold, sz_limit);
}

/*
 * Split a region in two
 *
 * r		the region to be split
 * sz_r		size of the first sub-region that will be made
 */
static void damon_split_region_at(struct damon_ctx *ctx,
				  struct damon_region *r, unsigned long sz_r)
{
	struct damon_region *new;

	new = damon_new_region(r->ar.start + sz_r, r->ar.end);
	if (!new)
		return;

	r->ar.end = new->ar.start;

	new->age = r->age;
	new->last_nr_accesses = r->last_nr_accesses;

	damon_insert_region(new, r, damon_next_region(r));
}

/* Split every region in the given target into 'nr_subs' regions */
static void damon_split_regions_of(struct damon_ctx *ctx,
				     struct damon_target *t, int nr_subs)
{
	struct damon_region *r, *next;
	unsigned long sz_region, sz_sub = 0;
	int i;

	damon_for_each_region_safe(r, next, t) {
		sz_region = r->ar.end - r->ar.start;

		for (i = 0; i < nr_subs - 1 &&
				sz_region > 2 * DAMON_MIN_REGION; i++) {
			/*
			 * Randomly select size of left sub-region to be at
			 * least 10 percent and at most 90% of original region
			 */
			sz_sub = ALIGN_DOWN(damon_rand(1, 10) *
					sz_region / 10, DAMON_MIN_REGION);
			/* Do not allow blank region */
			if (sz_sub == 0 || sz_sub >= sz_region)
				continue;

			damon_split_region_at(ctx, r, sz_sub);
			sz_region = sz_sub;
		}
	}
}

/*
 * Split every target region into randomly-sized small regions
 *
 * This function splits every target region into random-sized small regions if
 * current total number of the regions is equal or smaller than half of the
 * user-specified maximum number of regions.  This is for maximizing the
 * monitoring accuracy under the dynamically changeable access patterns.  If a
 * split was unnecessarily made, later 'kdamond_merge_regions()' will revert
 * it.
 */
static void kdamond_split_regions(struct damon_ctx *ctx)
{
	struct damon_target *t;
	unsigned int nr_regions = 0;
	static unsigned int last_nr_regions;
	int nr_subregions = 2;

	damon_for_each_target(t, ctx)
		nr_regions += damon_nr_regions(t);

	if (nr_regions > ctx->max_nr_regions / 2)
		return;

	/* Maybe the middle of the region has different access frequency */
	if (last_nr_regions == nr_regions &&
			nr_regions < ctx->max_nr_regions / 3)
		nr_subregions = 3;

	damon_for_each_target(t, ctx)
		damon_split_regions_of(ctx, t, nr_subregions);

	last_nr_regions = nr_regions;
}

/*
 * Check whether it is time to check and apply the target monitoring regions
 *
 * Returns true if it is.
 */
static bool kdamond_need_update_primitive(struct damon_ctx *ctx)
{
	return damon_check_reset_time_interval(&ctx->last_primitive_update,
			ctx->primitive_update_interval);
}

/*
 * Check whether current monitoring should be stopped
 *
 * The monitoring is stopped when either the user requested to stop, or all
 * monitoring targets are invalid.
 *
 * Returns true if need to stop current monitoring.
 */
static bool kdamond_need_stop(struct damon_ctx *ctx)
{
	struct damon_target *t;
	bool stop;

	mutex_lock(&ctx->kdamond_lock);
	stop = ctx->kdamond_stop;
	mutex_unlock(&ctx->kdamond_lock);
	if (stop)
		return true;

	if (!ctx->primitive.target_valid)
		return false;

	if (ctx->target_type == DAMON_ARBITRARY_TARGET)
		return !ctx->primitive.target_valid(ctx->arbitrary_target);

	damon_for_each_target(t, ctx) {
		if (ctx->primitive.target_valid(t))
			return false;
	}

	return true;
}

static void set_kdamond_stop(struct damon_ctx *ctx)
{
	mutex_lock(&ctx->kdamond_lock);
	ctx->kdamond_stop = true;
	mutex_unlock(&ctx->kdamond_lock);
}

/*
 * The monitoring daemon that runs as a kernel thread
 */
static int kdamond_fn(void *data)
{
	struct damon_ctx *ctx = (struct damon_ctx *)data;
	struct damon_target *t;
	struct damon_region *r, *next;
	unsigned int max_nr_accesses = 0;
	unsigned long sz_limit = 0;

	pr_info("kdamond (%d) starts\n", ctx->kdamond->pid);

	if (ctx->primitive.init)
		ctx->primitive.init(ctx);
	if (ctx->callback.before_start && ctx->callback.before_start(ctx))
		set_kdamond_stop(ctx);

	sz_limit = damon_region_sz_limit(ctx);

	while (!kdamond_need_stop(ctx)) {
		if (ctx->primitive.prepare_access_checks)
			ctx->primitive.prepare_access_checks(ctx);
		if (ctx->callback.after_sampling &&
				ctx->callback.after_sampling(ctx))
			set_kdamond_stop(ctx);

		usleep_range(ctx->sample_interval, ctx->sample_interval + 1);

		if (ctx->primitive.check_accesses)
			max_nr_accesses = ctx->primitive.check_accesses(ctx);

		if (kdamond_aggregate_interval_passed(ctx)) {
			if (ctx->target_type != DAMON_ARBITRARY_TARGET)
				kdamond_merge_regions(ctx,
						max_nr_accesses / 10,
						sz_limit);
			if (ctx->callback.after_aggregation &&
					ctx->callback.after_aggregation(ctx))
				set_kdamond_stop(ctx);
			if (ctx->target_type != DAMON_ARBITRARY_TARGET) {
				kdamond_apply_schemes(ctx);
				kdamond_reset_aggregated(ctx);
				kdamond_split_regions(ctx);
			}
			if (ctx->primitive.reset_aggregated)
				ctx->primitive.reset_aggregated(ctx);
		}

		if (kdamond_need_update_primitive(ctx)) {
			if (ctx->primitive.update)
				ctx->primitive.update(ctx);
			sz_limit = damon_region_sz_limit(ctx);
		}
	}
	if (ctx->target_type != DAMON_ARBITRARY_TARGET) {
		damon_for_each_target(t, ctx) {
			damon_for_each_region_safe(r, next, t)
				damon_destroy_region(r);
		}
	}

	if (ctx->callback.before_terminate &&
			ctx->callback.before_terminate(ctx))
		set_kdamond_stop(ctx);
	if (ctx->primitive.cleanup)
		ctx->primitive.cleanup(ctx);

	pr_debug("kdamond (%d) finishes\n", ctx->kdamond->pid);
	mutex_lock(&ctx->kdamond_lock);
	ctx->kdamond = NULL;
	mutex_unlock(&ctx->kdamond_lock);

	mutex_lock(&damon_lock);
	nr_running_ctxs--;
	mutex_unlock(&damon_lock);

	do_exit(0);
}

#include "core-test.h"
