#include "cache.h"
#include "sub-process.h"
#include "quote.h"
#include "trace2.h"

struct helper_process {
	struct subprocess_entry subprocess; /* must be first */
	unsigned int supported_capabilities;
};

struct helper_pool {
	/* we do not own the pointers within the array */
	struct helper_process **array;
	int nr, alloc;
};

#define CAP_EVERYTHING       (0u)

static int helper_start_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = {1, 0};
	static struct subprocess_capability capabilities[] = {
		{ NULL, 0 }
	};

	struct helper_process *hp = (struct helper_process *)subprocess;

	return subprocess_handshake(subprocess, "checkout-helper", versions,
				    NULL, capabilities,
				    &hp->supported_capabilities);
}

/*
 * The subprocess facility requires a hashmap to manage the children,
 * but I want an array to directly access a particular child, so we
 * have both a helper_pool and a helper_pool_map.  The map owns the
 * pointers.
 */
static struct helper_pool helper_pool;
static struct hashmap helper_pool_map;

static int helper_pool_initialized;

/*
 * Start a helper process.  The child_nr is there to force the sub-process
 * mechanism to let us have more than one instance of the same executable
 * (and to help with tracing).
 *
 * The returned helper_process pointer belongs to the map.
 */
static struct helper_process *find_or_start_checkout_helper(
	int child_nr,
	unsigned int cap_needed)
{
	struct helper_process *hp;
	struct argv_array argv = ARGV_ARRAY_INIT;
	struct strbuf quoted = STRBUF_INIT;

	argv_array_push(&argv, "checkout-helper");
	argv_array_pushf(&argv, "--child=%d", child_nr);

	sq_quote_argv_pretty(&quoted, argv.argv);

	if (!helper_pool_initialized) {
		helper_pool_initialized = 1;
		hashmap_init(&helper_pool_map, (hashmap_cmp_fn)cmd2process_cmp,
			     NULL, 0);
		hp = NULL;
	} else
		hp = (struct helper_process *)subprocess_find_entry(
			&helper_pool_map, quoted.buf);

	if (!hp) {
		hp = xcalloc(1, sizeof(*hp));
		hp->supported_capabilities = 0;

		if (subprocess_start_argv(&helper_pool_map, &hp->subprocess,
					  0, 1, &argv, helper_start_fn))
			FREE_AND_NULL(hp);
	}

	if (hp &&
	    (hp->supported_capabilities & cap_needed) != cap_needed) {
		error("checkout-helper does not support needed capabilities");
		subprocess_stop(&helper_pool_map, &hp->subprocess);
		FREE_AND_NULL(hp);
	}

	argv_array_clear(&argv);
	strbuf_release(&quoted);

	return hp;
}

int chc__launch_all_checkout_helpers(int nr_helpers_wanted)
{
	struct helper_process *hp;
	int err = 0;

	trace2_region_enter("pcheckout", "launch_all_helpers", NULL);

	ALLOC_GROW(helper_pool.array, nr_helpers_wanted, helper_pool.alloc);

	while (helper_pool.nr < nr_helpers_wanted) {
		hp = find_or_start_checkout_helper(helper_pool.nr, CAP_EVERYTHING);
		if (!hp) {
			err = 1;
			break;
		}

		helper_pool.array[helper_pool.nr++] = hp;
	}

	trace2_region_leave("pcheckout", "launch_all_helpers", NULL);

	return err;
}

/*
 * Cause all of our checkout-helper processes to exit.
 *
 * Closing our end of the pipe to their STDIN causes the server_loop
 * to terminate and let the child exit normally.  We leave the actual
 * zombie-reaping to the atexit() routines in run-command.c to save
 * time -- their shutdown can happen in parallel with the rest of our
 * checkout computation; that is, there is no need for us to wait()
 * for them right now.
 *
 * Also, this method is faster than calling letting subprocess_stop()
 * send a kill(SIGTERM) to the child and then wait() for it.
 */
void chc__stop_all_checkout_helpers(void)
{
	struct helper_process *hp;
	int k;

	trace2_region_enter("pcheckout", "stop_helpers", NULL);

	for (k = 0; k < helper_pool.nr; k++) {
		hp = helper_pool.array[k];

		close(hp->subprocess.process.in);

		/* The pool does not own the pointer. */
		helper_pool.array[k] = NULL;
	}

	trace2_region_leave("pcheckout", "stop_helpers", NULL);

	FREE_AND_NULL(helper_pool.array);
	helper_pool.nr = 0;
	helper_pool.alloc = 0;
}
