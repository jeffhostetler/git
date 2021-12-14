#ifndef TR2_SW_H
#define TR2_SW_H

#include "trace2/tr2_tgt.h"

/*
 * Define a mechanism to allow "stopwatch" timers.
 *
 * Timers can be used to measure "interesting" activity that does not
 * fit the "region" model, such as code called from many different
 * regions (like zlib) and/or where data for individual calls are not
 * interesting or are too numerous to be efficiently logged.
 *
 * Timer values are accumulated during program execution and emitted
 * to the Trace2 logs at program exit.
 *
 * To make this model efficient, we define a compile-time fixed set of
 * timers and timer ids.  This lets us avoid the complexities of
 * dynamically allocating a timer on demand and sharing that
 * definition with other threads.
 *
 * Timer values are stored in a fixed size "timer block" inside the
 * TLS CTX.  This allows data to be collected on a thread-by-thread
 * basis without locking.
 *
 * We define (at compile time) a set of "timer ids" to access the
 * various timers inside the fixed size "timer block".
 *
 * Timer definitions include the Trace2 "category" and similar fields.
 * This eliminates the need to include those args on the various timer
 * APIs.
 *
 * Timer results are summarized and emitted by the main thread at
 * program exit by iterating over the global list of CTX data.
 */

/*
 * The definition of an individual timer and used by an individual
 * thread.
 */
struct tr2sw_timer {
	/*
	 * Total elapsed time for this timer in this thread.
	 */
	uint64_t elapsed_us;

	/*
	 * The maximum and minimum interval values observed for this
	 * timer in this thread.
	 */
	uint64_t min_us;
	uint64_t max_us;

	/*
	 * The value of the clock when this timer was started in this
	 * thread.  (Undefined when the timer is not active in this
	 * thread.)
	 */
	uint64_t start_us;

	/*
	 * Number of times that this timer has been started and stopped
	 * in this thread.  (Recursive starts are ignored.)
	 */
	size_t interval_count;

	/*
	 * Number of nested starts on the stack in this thread.  (We
	 * ignore recursive starts and use this to track the recursive
	 * calls.)
	 */
	size_t recursion_count;
};

/*
 * A compile-time fixed-size block of timers to insert into the TLS CTX.
 *
 * We use this simple wrapper around the array of timer instances to
 * avoid C syntax quirks and the need to pass around an additional size_t
 * argument.
 */
struct tr2sw_timer_block {
	struct tr2sw_timer timer[TRACE2_SW_ID__MUST_BE_LAST];
};

void tr2sw_start(enum trace2_stopwatch_id swid);
void tr2sw_stop(enum trace2_stopwatch_id swid);

/*
 * Add timer data from "sw" into "sw_merged".
 */
void tr2sw_merge(struct tr2sw_timer_block *sw_merged,
		 const struct tr2sw_timer_block *sw);

/*
 * Send stopwatch data for all of the timers to the TGT.
 */
void tr2sw_emit_timer_block(tr2_tgt_evt_stopwatch_t *pfn,
			    uint64_t us_elapsed_absolute,
			    const struct tr2sw_timer_block *sw);

#endif /* TR2_SW_H */
