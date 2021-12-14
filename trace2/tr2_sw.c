#include "cache.h"
#include "thread-utils.h"
#include "trace2/tr2_tls.h"
#include "trace2/tr2_sw.h"

/*
 * Metadata for each individual timer within our timer block.
 * This list must match the ID enum values.
 */
static struct trace2_stopwatch_defs tr2sw_defs[] = {
	{ TRACE2_SW_ID__TEST, "test", "test" },
};

void tr2sw_start(enum trace2_stopwatch_id swid)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
	struct tr2sw_timer *t = &ctx->sw.timer[swid];

	t->recursion_count++;
	if (t->recursion_count > 1)
		return; /* ignore recursive starts */

	t->start_us = getnanotime() / 1000;
}

void tr2sw_stop(enum trace2_stopwatch_id swid)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
	struct tr2sw_timer *t = &ctx->sw.timer[swid];
	uint64_t us_now;
	uint64_t us_interval;

	assert(t->recursion_count > 0);

	t->recursion_count--;
	if (t->recursion_count > 0)
		return; /* still in recursive call */

	us_now = getnanotime() / 1000;
	us_interval = us_now - t->start_us;

	t->elapsed_us += us_interval;

	if (!t->interval_count) {
		t->min_us = us_interval;
		t->max_us = us_interval;
	} else {
		if (us_interval < t->min_us)
			t->min_us = us_interval;
		if (us_interval > t->max_us)
			t->max_us = us_interval;
	}

	t->interval_count++;
}

void tr2sw_merge(struct tr2sw_timer_block *sw_merged,
		 const struct tr2sw_timer_block *sw)
{
	enum trace2_stopwatch_id id;

	for (id = 0; id < TRACE2_SW_ID__MUST_BE_LAST; id++) {
		struct tr2sw_timer *t_merged = &sw_merged->timer[id];
		const struct tr2sw_timer *t = &sw->timer[id];

		if (t->recursion_count > 0) {
			/*
			 * A thread exited with a stopwatch running.
			 *
			 * NEEDSWORK: should we assert or throw a warning
			 * for the open interval.  I'm going to ignore it
			 * and keep going because we may have valid data
			 * for previously closed intervals on this timer.
			 */
		}

		if (!t->interval_count)
			continue; /* sw was not used by this thread. */

		t_merged->elapsed_us += t->elapsed_us;

		if (!t_merged->interval_count) {
			t_merged->min_us = t->min_us;
			t_merged->max_us = t->max_us;
		} else {
			if (t->min_us < t_merged->min_us)
				t_merged->min_us = t->min_us;
			if (t->max_us > t_merged->max_us)
				t_merged->max_us = t->max_us;
		}

		t_merged->interval_count += t->interval_count;
	}
}

void tr2sw_emit_timer_block(tr2_tgt_evt_stopwatch_t *pfn,
			    uint64_t us_elapsed_absolute,
			    const struct tr2sw_timer_block *sw)
{
	enum trace2_stopwatch_id id;

	for (id = 0; id < TRACE2_SW_ID__MUST_BE_LAST; id++) {
		const struct tr2sw_timer *t = &sw->timer[id];

		if (!t->interval_count)
			continue; /* this timer was not used */

		pfn(us_elapsed_absolute,
		    tr2sw_defs[id].category, tr2sw_defs[id].name,
		    t->interval_count, t->elapsed_us, t->min_us, t->max_us);
	}
}
