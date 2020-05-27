#include "cache.h"
#include "sub-process.h"
#include "quote.h"
#include "trace2.h"
#include "checkout-helper-client.h"
#include "checkout-helper.h"

int chc__get_value__is_enabled(void)
{
	static int is_enabled = -1;
	const char *value;

	if (is_enabled < 0) {

		is_enabled = 0;

		/* TODO Get value from config, if set. */

		value = getenv(GIT_TEST_CHECKOUT_HELPER);
		if (value)
			is_enabled = strtol(value, NULL, 10);

		if (is_enabled < 0)
			is_enabled = 0;
	}

	return is_enabled;
}

int chc__get_value__threshold(void)
{
	static int threshold = -1;
	const char *value;

	if (threshold < 0) {

		threshold = 1; /* TODO increase this */

		/* TODO Get value from config, if set. */

		value = getenv(GIT_TEST_CHECKOUT_HELPER_THRESHOLD);
		if (value)
			threshold = strtol(value, NULL, 10);

		if (threshold < 1)
			threshold = 1;
	}

	return threshold;
}

int chc__get_value__helpers_wanted(void)
{
	static int wanted = -1;
	const char *value;

	if (wanted < 0) {

		/* Arbitrary choice to be nice */
		/* TODO Revisit this */
		wanted = online_cpus() / 3;

		/* TODO Get value from config, if set. */

		value = getenv(GIT_TEST_CHECKOUT_HELPER_COUNT);
		if (value)
			wanted = strtol(value, NULL, 10);

		if (wanted < 1)
			wanted = 1;
	}

	return wanted;
}


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

/*
 * Some types of smudging cannot be handled by the checkout-helper.
 */
static int is_chc_eligible(const struct conv_attrs *ca)
{
	enum conv_attrs_classification c = classify_conv_attrs(ca);

	switch (c) {
	default:
		BUG("unsupported conv_attrs classification '%d'", c);
		return 0;

	case CA_CLASS_INCORE:
		return 1;

	case CA_CLASS_INCORE_FILTER:
		/*
		 * It would be safe to allow concurrent instances of
		 * single-file smudge filters, like rot13, but we should
		 * not assume that all filters are parallel-process safe.
		 * So we don't allow this.
		 */
		return 0;

	case CA_CLASS_INCORE_PROCESS:
		/*
		 * The parallel queue and the delayed queue are not
		 * compatible and must be keep completely separate.
		 * So "process" filtered items are not eligible for
		 * parallel checkout.
		 *
		 * The process filter may return the blob content on the
		 * initial request or it may return a "delayed" response
		 * and not return the blob content until the CE_RETRY
		 * request.  And we can't tell which without actually
		 * asking the long-running process filter to do it.
		 *
		 * Furthermore, there should only be one instance of
		 * the long-running process filter because we don't know
		 * how it is managing its own concurrency.  And if we
		 * spread the parallel queue across multiple threads or
		 * processes, each would need to talk to their own
		 * instance of the process filter and that could cause
		 * problems.
		 */
		return 0;

	case CA_CLASS_STREAMABLE:
		return 1;
	}
}

/*
 * A `chc_item` serves as a bridge between the `cache_entry` and
 * the data we send to a `checkout-helper` process and the results
 * from the helper.
 */
struct chc_item {
	/*
	 * Back pointer to the cache_entry in istate->cache[].
	 * We do not own this.
	 */
	struct cache_entry *ce;

	struct conv_attrs ca;

	/*
	 * The position of this item within chc_data.items[].
	 * (It may not match the position in index.cache_entries[]
	 * because not all cache-entries are "eligible" for parallel
	 * checkout.
	 */
	int chc_item_nr;

	/*
	 * The number of the child helper process that we queued
	 * this item to.  Set to -1 if not yet assigned.
	 */
	int child_nr;

	/*
	 * The item number that the child process knows it as.
	 * This is a contiguous series WRT that child.  Set to
	 * -1 if not yet assigned.
	 */
	int helper_item_nr;

	/*
	 * The error { <class>, <errno> } received from the helper.
	 * This allows the foreground process to report errors with
	 * context later or ignore them as it sees fit, because the
	 * helper process cannot report does not have the context
	 * and synchronization to report individual errors on blobs.
	 */
	enum checkout_helper__item_error_class item_error_class;
	int item_errno;
};

struct chc_data {
	struct chc_item **items;
	int nr, alloc;

	struct strbuf base_dir;
};

static struct chc_item *append_chc_item(struct chc_data *chc_data,
					struct cache_entry *ce,
					const struct conv_attrs *ca)
{
	struct chc_item *item = xcalloc(1, sizeof(*item));

	/*
	 * We borrow the {name, oid} from the ce when we talk to
	 * the helper.
	 */
	assert(ce != NULL);
	assert(ce->name && *ce->name);
	assert(!is_null_oid(&ce->oid));

	/*
	 * The ce and this item need to be bound together for the
	 * duration of the helper operation, so it must not already
	 * have an associated item.
	 */
	assert(ce->chc_item == NULL);

	item->ce = ce;
	ce->chc_item = item;

	item->ca.drv = ca->drv;
	item->ca.attr_action = ca->attr_action;
	item->ca.crlf_action = ca->crlf_action;
	item->ca.ident = ca->ident;
	if (ca->working_tree_encoding && *ca->working_tree_encoding)
		item->ca->working_tree_encoding =
			strdup(ca->working_tree_encoding);

	item->chc_item_nr = chc_data->nr;
	item->child_nr = -1;
	item->helper_item_nr = -1;

	item->item_error_class = IEC__NO_RESULT;
	item->item_errno = 0;

	ALLOC_GROW(chc_data->items, chc_data->nr + 1, chc_data->alloc);
	chc_data->items[chc_date->nr++] = item;

	return item;
}

static void free_chc_item(struct chc_item *item)
{
	if (!item)
		return;

	if (item->ce && item == item->ce->chc_item) {
		item->ce = NULL;
		item->ce->chc_item = NULL;
	}

	free((char *)item->ca.working_tree_encoding);
	free(item);
}

void chc__free_data(struct chc_data *chc_data)
{
	int k;

	if (!chc_data)
		return;

	for (k = 0; k < chc_data->nr; k++)
		free_chc_item(chc_data->items[k]);

	strbuf_release(&chc_data->base_dir);

	free(chc_data);
}

struct chc_data *chc__alloc_data(const char *base_dir, int base_dir_len)
{
	struct chc_data *chc_data = xcalloc(1, sizeof(*chc_data));

	strbuf_init(&chc_data->base_dir, 0);
	strbuf_add(&chc_data->base_dir, base_dir, base_dir_len);

	return chc_data;
}

/*
 * First-order approximation of the total amount of work required.
 *
 * In this loop, we only count the regular files that need updating
 * (and without regard to eligibility) because this needs to be a
 * fast threshold scan.
 */
static int count_approximate_items(struct index_state *istate)
{
	int k;
	int nr_updated = 0;

	for (k = 0; k < istate->cache_nr; k++) {
		const struct cache_entry *ce = istate->cache[k];

		if (!(ce->ce_flags & CE_UPDATE))
			continue;

		if ((ce->ce_mode & S_IFMT) != S_IFREG)
			continue;

		if (ce->ce_flags & CE_WT_REMOVE)
			BUG("both update and delete flags are set on %s",
			    ce->name);

		nr_updated++;
	}

	return nr_updated;
}

/*
 * Append the set of "parallel eligible" files that need to be updated
 * to `chc_data`.
 *
 * Use this sequential pass thru the index to capture the smudging rules
 * required for each file in advance of actually needing them.  This
 * depth-first traversal allows us to evaluate the "attribute stack" using
 * the traditional mechanisms and without locking or concurrency concerns
 * on the stack itself.
 */
void chc__append_eligible_items(struct chc_data *chc_data,
			       struct index_state *istate)
{
	int k;

	trace2_region_enter("pcheckout", "append_eligible", NULL);

	for (k = 0; k < istate->cache_nr; k++) {
		struct cache_entry *ce = istate->cache[k];
		struct chc_item *item;
		struct conv_attrs ca;

		if (!(ce->ce_flags & CE_UPDATE))
			continue;
		if ((ce->ce_mode & S_IFMT) != S_IFREG)
			continue;

		convert_attrs(istate, &ca, ce->name);
		if (!is_chc_eligible(&ca))
			continue;

		item = append_chc_item(chc_data, ce, &ca);
	}

	trace2_region_enter("pcheckout", "append_eligible", NULL);
}

struct chc_data *chc__setup_data(struct checkout *state,
				 struct unpack_trees_options *o)
