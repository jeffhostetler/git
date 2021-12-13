#include "tr2_timer.h"

/*
 * A per-thread instance of a timer.
 */
struct tr2timer_thread_instance {
	/*
	 * Total elapsed time for this timer in this thread.
	 */
	uint64_t elapsed_us;

	/*
	 * The maximum and minimum interval values observed in
	 * the current thread.
	 */
	uint64_t max_us;
	uint64_t min_us;

	/*
	 * If non-zero, the timer is active in the current thread.
	 * This records the clock value when the timer was last
	 * started.
	 *
	 * If zero, means that the timer is not currently running
	 * in this thread.
	 *
	 * Recursive calls to start this timer will be ignored and
	 * only the outer-most _start().._stop() span will be
	 * measured.
	 */
	uint64_t start_us;

	/*
	 * Number of times that this timer has been started and
	 * stopped in this thread.
	 *
	 * Again, recursive calls are ignored.
	 */
	size_t interval_count;

	/*
	 * Used to identify recursive calls so that they can be
	 * ignored.
	 */
	size_t recursion_count;
};

/*
 * A block of timers associated with a single thread.  This will
 * be inserted into the thread's TLS ctx data.
 *
 * A simple wrapper around the block of timer instances to avoid
 * C syntax quirks and the need to pass around an additional size_t
 * argument.
 */
struct tr2timer_thread_timers {
	struct tr2timer_thead_instance timers[TRACE2_TIMER__MUST_BE_LAST];
};

/*
 * Define an array of timer definitions.  These provide the metadata
 * to describe each timer.  These are universal properties for the
 * timer.
 *
 * Since timers are defined at compile time, we can have simple _start()
 * and _stop() methods in the API without passing the metadata on each
 * call.
 */
struct tr2timer_definition {
	enum trace2_timer_id id;   /* safety check */

	const char *category;      /* category name */
	const char *name;          /* timer name */

	uint64_t features;         /* see TF_* */
};

struct tr2timer_defs {
	struct tr2timer_definition defs[TRACE2_TIMER__MUST_BE_LAST];
};


/* Emit timer event when even when count is zero. */
#define TF_WANT_ZERO_EVENT          (1<<0)

/* Emit timer event when each thread exits. */
#define TF_WANT_THREAD_EVENT        (1<<1)

/* Emit timer event when the process exits. */
#define TF_WANT_PROCESS_EVENT       (1<<2)

#define TIMER_DEF(id, cat, name, tf)		\
	{					\
		.id = (id),			\
		.category = (cat)		\
		.name = (name),			\
		.features = (tf)		\
	}

static struct tr2timer_defs tr2_timers = {
	TIMER_DEF(TRACE2_TIMER__ZIP_DEFLATE, "zip", "deflate",
		  TF_WANT_PROCESS_EVENT),
	TIMER_DEF(TRACE2_TIMER__ZIP_INFLATE, "zip", "inflate",
		  TF_WANT_PROCESS_EVENT),
};


