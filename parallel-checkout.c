#include "cache.h"
#include "blob.h"
#include "object-store.h"
#include "dir.h"
#include "streaming.h"
#include "submodule.h"
#include "progress.h"
#include "fsmonitor.h"
#include "argv-array.h"
#include "pkt-line.h"
#include "quote.h"
#include "sigchain.h"
#include "sub-process.h"
#include "trace2.h"
#include "parallel-checkout.h"
#include "checkout--helper.h"
#include "thread-utils.h"
#include "unpack-trees.h"

int is_eligible_for_parallel_checkout(const struct conv_attrs *ca)
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

enum helper_spread_model {
	/*
	 * Spread the parallel-eligible items "horizontally" across
	 * all of the helper processes.  For example, if we have h=10
	 * helpers, helper[k] will receive {k, k+10, k+20, ...}.
	 *
	 * This should help spread the work of preloading the first h
	 * blobs from the ODB as quickly as possible and should help
	 * when sequentially populating the working directory.
	 */
	HSM__HORIZONTAL = 0,

	/*
	 * Spread the parallel-eligible items "vertically" across all
	 * of the helper processes.  This is the normal way to slice
	 * the array of items.
	 *
	 * This may help reduce kernel lock contention on individual
	 * directories when populating items in a fully-parallel fashion
	 * since helpers should be in different parts of the working
	 * directory and (usually) not competing to add files to the
	 * same sub-directory.
	 */
	HSM__VERTICAL,
};

static int nr_helper_processes_wanted;
static int nr_writer_threads;
static int nr_preloads;

static int helper_pool_initialized;

struct helper_process {
	struct subprocess_entry subprocess; /* must be first */
	unsigned int supported_capabilities;

	/* the number of items sent to this helper */
	int helper_item_count;

	/* the number of items for which we've received results */
	int helper_result_count;

	/* don't try to talk to helper again after an IO error */
	int helper_is_dead_to_us;
};

struct helper_pool {
	/* we do not own the pointers within the array */
	struct helper_process **array;
	int nr, alloc;
};

/*
 * The subprocess facility requires a hashmap to manage the children,
 * but I want an array to directly access a particular child, so we
 * have both a helper_pool and a helper_pool_map.  The map owns the
 * pointers.
 */
static struct helper_pool helper_pool;
static struct hashmap helper_pool_map;

struct parallel_checkout_item {
	/*
	 * Back pointer to the cache_entry in istate->cache[].
	 * We do not own this.
	 */
	struct cache_entry *ce;

	struct conv_attrs ca;

	/*
	 * The position of this item within parallel_checkout.items[].
	 * (It may not match the position in index.cache_entries[]
	 * because not all cache-entries are "eligible" for parallel
	 * checkout.
	 */
	int pc_item_nr;
	/*
	 * The number of the child helper process that we queued this item to.
	 */
	int child_nr;
	/*
	 * The item number that the child process knows it as.  This is a
	 * contiguous series WRT that child.
	 */
	int helper_item_nr;

	/*
	 * Used in asynch mode to indicate that the progress meter has
	 * been advanced for this item.  Usually, this means that the helper
	 * has successfully populated the item.  (Or where there's a hard
	 * error such that the classic code fallback wouldn't help.)
	 *
	 * Usually this is set when the CE_UPDATE bit is turned off.
	 */
	unsigned progress_claimed:1;

	/*
	 * Remember the error type and value received from the helper.
	 */
	enum checkout_helper__item_error_class item_error_class;
	int item_errno;
};

struct parallel_checkout {
	struct parallel_checkout_item **items;
	int nr, alloc;
	struct strbuf base_dir;
	enum parallel_checkout_mode pcm;
	enum helper_spread_model hsm;
};

int is_parallel_checkout_mode(const struct checkout *state,
			      enum parallel_checkout_mode mode)
{
	if (!state->parallel_checkout)
		return mode == PCM__NONE;

	return state->parallel_checkout->pcm == mode;
}

static void free_parallel_checkout_item(struct parallel_checkout_item *item)
{
	if (!item)
		return;

	free((char *)item->ca.working_tree_encoding);
	free(item);
}

static void free_parallel_checkout(struct parallel_checkout *pc)
{
	int k;

	if (!pc)
		return;

	for (k = 0; k < pc->nr; k++)
		free_parallel_checkout_item(pc->items[k]);

	strbuf_release(&pc->base_dir);

	free(pc);
}

/*
 * Print an error message describing why the helper associated with
 * this item could not create the file.  This lets the foreground
 * process control the ordering of messages to better match the
 * order from the classic (sequential) version.
 */
static void parallel_checkout__print_helper_error(
	struct parallel_checkout_item *item)
{
	switch (item->item_error_class) {
	case IEC__NO_RESULT:
	case IEC__OK:
		return;

	case IEC__INVALID_ITEM:
		/*
		 * We asked helper[k] for an item that we did not send it.
		 */
		error("Invalid item for helper[%d] '%s'",
		      item->child_nr, item->ce->name);
		return;

	case IEC__LOAD:
		error("error loading blob for '%s': %s",
		      item->ce->name, strerror(item->item_errno));
		return;

	case IEC__OPEN:
		error("error creating file '%s': %s",
		      item->ce->name, strerror(item->item_errno));
		return;

	case IEC__WRITE:
		error("error writing to file '%s': %s",
		      item->ce->name, strerror(item->item_errno));
		return;

	case IEC__LSTAT:
		error("error stating file '%s': %s",
		      item->ce->name, strerror(item->item_errno));
		return;

	default:
		error("Invalid IEC for file '%s': %d",
		      item->ce->name, item->item_error_class);
		return;
	}
}

#define CAP_QUEUE          (1u<<1)
#define CAP_SYNC_WRITE     (1u<<2)
#define CAP_ASYNC_PROGRESS (1u<<3)
#define CAP_EVERYTHING     (CAP_QUEUE | CAP_SYNC_WRITE | CAP_ASYNC_PROGRESS)

#define CAP_QUEUE_NAME          "queue"
#define CAP_SYNC_WRITE_NAME     "sync_write"
#define CAP_ASYNC_PROGRESS_NAME "async_progress"

static int helper_start_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = {1, 0};
	static struct subprocess_capability capabilities[] = {
		{ CAP_QUEUE_NAME, CAP_QUEUE },
		{ CAP_SYNC_WRITE_NAME, CAP_SYNC_WRITE },
		{ CAP_ASYNC_PROGRESS_NAME, CAP_ASYNC_PROGRESS },
		{ NULL, 0 }
	};

	struct helper_process *hp = (struct helper_process *)subprocess;

	return subprocess_handshake(subprocess, "checkout--helper", versions,
				    NULL, capabilities,
				    &hp->supported_capabilities);
}

/*
 * Start a helper process.  The child_nr is there to force the subprocess
 * mechanism to let us have more than one instance of the same executable
 * (and to help with tracing).
 *
 * The returned helper_process pointer belongs to the map.
 */
static struct helper_process *helper_find_or_start_process(
	struct parallel_checkout *pc,
	unsigned int cap_needed,
	int child_nr)
{
	struct helper_process *hp;
	struct argv_array argv = ARGV_ARRAY_INIT;
	struct strbuf quoted = STRBUF_INIT;

	argv_array_push(&argv, "checkout--helper");
	argv_array_pushf(&argv, "--child=%d", child_nr);
	argv_array_pushf(&argv, "--writers=%d", nr_writer_threads);
	argv_array_pushf(&argv, "--preload=%d", nr_preloads);

	if (pc->pcm == PCM__ASYNCHRONOUS)
		argv_array_push(&argv, "--asynch");
	else if (pc->pcm == PCM__SYNCHRONOUS)
		argv_array_push(&argv, "--no-asynch");

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
		error("helper does not support needed capabilities");
		subprocess_stop(&helper_pool_map, &hp->subprocess);
		FREE_AND_NULL(hp);
	}

	argv_array_clear(&argv);
	strbuf_release(&quoted);

	return hp;
}

/*
 * Cause all of our checkout--helper processes to exit.
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
static void stop_all_helpers(void)
{
	struct helper_process *hp;
	int k;

	trace2_region_enter("pcheckout", "stop_helpers", NULL);

	for (k = 0; k < helper_pool.nr; k++) {
		hp = helper_pool.array[k];

		close(hp->subprocess.process.in);

		hp->helper_is_dead_to_us = 1;

		/* The pool does not own the pointer. */
		helper_pool.array[k] = NULL;
	}

	trace2_region_leave("pcheckout", "stop_helpers", NULL);

	FREE_AND_NULL(helper_pool.array);
	helper_pool.nr = 0;
	helper_pool.alloc = 0;
}

static void send__queue_item_record(struct parallel_checkout *pc,
				    int pc_item_nr, int child_nr)
{
	struct parallel_checkout_item *item = pc->items[pc_item_nr];
	struct conv_attrs *ca = &item->ca;
	struct helper_process *hp = helper_pool.array[child_nr];
	struct child_process *process = &hp->subprocess.process;
	uint32_t len_name;
	uint32_t len_encoding_name;
	char *data = NULL;
	size_t len_data;
	struct checkout_helper__queue_item_record *fixed_fields;
	char *variant;

	/*
	 * As described in is_eligible_for_parallel_checkout(), we
	 * don't consider cache-entries that need an external tool to
	 * be eligible.  So they should always have a null driver.  If
	 * we decide to change this, we would also need to send the
	 * driver fields to the helper.
	 */
	if (ca->drv)
		BUG("ineligible cache-entry '%s'", item->ce->name);

	assert(pc_item_nr == item->pc_item_nr);

	/*
	 * Update the item to remember with whom we queued it.
	 */
	item->child_nr = child_nr;
	item->helper_item_nr = hp->helper_item_count++;

	/*
	 * Build binary record to send to the helper in 1 message
	 * that won't require a lot of parsing on their side.
	 */
	len_name = pc->base_dir.len + item->ce->ce_namelen;
	assert(len_name > 0);

	if (ca->working_tree_encoding && *ca->working_tree_encoding)
		len_encoding_name = strlen(ca->working_tree_encoding);
	else
		len_encoding_name = 0;

	len_data = sizeof(struct checkout_helper__queue_item_record) +
		len_name + len_encoding_name;

	data = xcalloc(1, len_data);

	fixed_fields = (struct checkout_helper__queue_item_record*)data;
	fixed_fields->pc_item_nr = item->pc_item_nr;
	fixed_fields->helper_item_nr = item->helper_item_nr;
	fixed_fields->ce_mode = item->ce->ce_mode;
	fixed_fields->attr_action = ca->attr_action;
	fixed_fields->crlf_action = ca->crlf_action;
	fixed_fields->ident = ca->ident;
	fixed_fields->len_name = len_name;
	fixed_fields->len_encoding_name = len_encoding_name;
	oidcpy(&fixed_fields->oid, &item->ce->oid);

	variant = data + sizeof(*fixed_fields);

	/*
	 * These string buffers are not null terminated, since we
	 * have length fields in the fixed portion of the record.
	 */
	if (len_encoding_name) {
		memcpy(variant, ca->working_tree_encoding, len_encoding_name);
		variant += len_encoding_name;
	}

	memcpy(variant, pc->base_dir.buf, pc->base_dir.len);
	variant += pc->base_dir.len;
	memcpy(variant, item->ce->name, item->ce->ce_namelen);
	variant += item->ce->ce_namelen;

	packet_write(process->in, data, len_data);

	free(data);
}

static void debug_dump_item(const struct parallel_checkout_item *item,
			    const char *label,
			    const struct stat *st)
{
#if 0
	trace2_printf(
		"%s: h[%d] item[%d,%d] e[%d,%d] st[%d,%d]",
		label,

		item->child_nr,

		item->pc_item_nr,
		item->helper_item_nr,

		item->item_error_class,
		item->item_errno,

		st->st_size,
		st->st_mtime);
#endif
}

/*
 * Send a "sync_write" command to ask the helper to write this
 * item to the work tree and receive the result data.
 *
 * We expect the response to describe exactly 1 item (not a range).
 *
 * Return 1 if we have a packet IO error.
 */
static int send_cmd__sync_write(struct parallel_checkout_item *item,
				struct stat *st)
{
	struct helper_process *hp = helper_pool.array[item->child_nr];
	struct child_process *process = &hp->subprocess.process;
	struct checkout_helper__sync__write_record rec;
	const struct checkout_helper__item_result *temp;
	char *line;
	int len;

	memset(&rec, 0, sizeof(rec));
	rec.helper_item_nr = item->helper_item_nr;

	packet_write_fmt_gently(process->in, "command=%s\n",
				CAP_SYNC_WRITE_NAME);
	packet_write(process->in, (const char *)&rec, sizeof(rec));
	if (packet_flush_gently(process->in)) {
		hp->helper_is_dead_to_us = 1;
		return 1;
	}

	len = packet_read_line_gently(process->out, NULL, &line);
	if (len < 0 || !line)
		BUG("sync_write: premature flush or EOF");

	if (len != sizeof(struct checkout_helper__item_result))
		BUG("sync_write: wrong length (obs %d, exp %d)",
		    len, (int)sizeof(struct checkout_helper__item_result));

	temp = (const struct checkout_helper__item_result *)line;

	item->item_error_class = temp->item_error_class;
	item->item_errno = temp->item_errno;
	memcpy(st, &temp->st, sizeof(struct stat));

	debug_dump_item(item, "sync-write", st);

	if (temp->helper_item_nr != item->helper_item_nr)
		BUG("sync_write: h[%d] wrong item req[%d,%d] rcv[%d,%d]",
		    item->child_nr,
		    item->pc_item_nr, item->helper_item_nr,
		    temp->pc_item_nr, temp->helper_item_nr);
	if (temp->pc_item_nr != item->pc_item_nr ||
	    temp->item_error_class == IEC__INVALID_ITEM)
		BUG("sync_write: h[%d] unk item req[%d,%d] rcv[%d,%d]",
		    item->child_nr,
		    item->pc_item_nr, item->helper_item_nr,
		    temp->pc_item_nr, temp->helper_item_nr);

	/* eat the flush packet */
	while (1) {
		len = packet_read_line_gently(process->out, NULL, &line);
		if (len < 0 || !line)
			break;
	}

	return 0;
}

/*
 * To helper[k] we send:
 *     command=queue<LF>
 *     <binary data for item>
 *     <binary data for item>
 *     ...
 *     <flush>
 *
 * Where each record looks like:
 *     <fixed-fields>[<encoding>]<path>
 */
static int send_items_to_helpers(struct parallel_checkout *pc)
{
	int child_nr;
	int pc_item_nr;
	int nr, k;
	int err = 0;

	trace2_region_enter("pcheckout", "send_items", NULL);

	/*
	 * Begin a queue command with each helper in parallel.
	 */
	for (child_nr = 0; child_nr < helper_pool.nr; child_nr++) {
		struct helper_process *hp = helper_pool.array[child_nr];
		struct child_process *process = &hp->subprocess.process;

		if (packet_write_fmt_gently(process->in, "command=%s\n",
					    CAP_QUEUE_NAME)) {
			hp->helper_is_dead_to_us = 1;
			err = 1;
			goto done;
		}
	}

	/*
	 * Distribute the array of items to the helpers as part of
	 * the queue command that we've opened.
	 */
	switch (pc->hsm) {
	case HSM__HORIZONTAL:
		for (pc_item_nr = 0; pc_item_nr < pc->nr; pc_item_nr++) {
			child_nr = pc_item_nr % helper_pool.nr;

			send__queue_item_record(pc, pc_item_nr, child_nr);
		}
		break;

	case HSM__VERTICAL:
		pc_item_nr = 0;
		nr = DIV_ROUND_UP(pc->nr, helper_pool.nr);
		for (child_nr = 0; child_nr < helper_pool.nr; child_nr++) {
			for (k = 0;
			     pc_item_nr < pc->nr && k < nr;
			     pc_item_nr++, k++) {
				send__queue_item_record(pc, pc_item_nr, child_nr);
			}
		}
		break;

	default:
		BUG("unknown helper_spread_model %d", (int)pc->hsm);
		break;
	}

	/*
	 * close the queue command with each helper.
	 */
	for (child_nr = 0; child_nr < helper_pool.nr; child_nr++) {
		struct helper_process *hp = helper_pool.array[child_nr];
		struct child_process *process = &hp->subprocess.process;

		if (packet_flush_gently(process->in)) {
			hp->helper_is_dead_to_us = 1;
			err = 1;
			goto done;
		}
	}

done:
	trace2_region_leave("pcheckout", "send_items", NULL);

	return err;
}

static int launch_all_helpers(struct parallel_checkout *pc)
{
	struct helper_process *hp;
	int err = 0;

	trace2_region_enter("pcheckout", "launch_helpers", NULL);

	ALLOC_GROW(helper_pool.array, nr_helper_processes_wanted, helper_pool.alloc);

	while (helper_pool.nr < nr_helper_processes_wanted) {
		hp = helper_find_or_start_process(pc, CAP_EVERYTHING, helper_pool.nr);
		if (!hp) {
			err = 1;
			break;
		}

		helper_pool.array[helper_pool.nr++] = hp;
	}

	trace2_region_leave("pcheckout", "launch_helpers", NULL);

	return err;
}

/*
 * Is parallel-checkout enabled?
 *
 * Let environment variable override the config setting so that we
 * can force in on in the test suite.
 */
int parallel_checkout_enabled(void)
{
	static int is_enabled = -1;

	if (is_enabled < 0) {
		const char *value = getenv("GIT_TEST_PARALLEL_CHECKOUT");
		if (value)
			is_enabled = strtol(value, NULL, 10);
		else
			is_enabled = core_parallel_checkout;
	}

	return is_enabled;
}

/*
 * Get parallel-checkout threshold.
 *
 * Let environment variable override the config setting so that we
 * can force a value in the test suite.
 */
int parallel_checkout_threshold(void)
{
	static int threshold = -1;

	if (threshold < 0) {
		const char *value;

		threshold = core_parallel_checkout_threshold;

		value = getenv("GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD");
		if (value) {
			int ivalue = strtol(value, NULL, 10);
			if (ivalue > 0)
				threshold = ivalue;
		}
	}

	return threshold;
}

/*
 * TODO evaluate the current command args to determine whether we
 * should run sync or async.  For example, in a clone or when extending
 * a sparse-checkout (and we expect to be newly populating an empty
 * worktree), we can run async.  And if we are switching branches and
 * need to look for dirty files before overwriting them, we should run
 * in sync mode.
 */
static enum parallel_checkout_mode compute_best_pcm(
	struct checkout *state,
	struct unpack_trees_options *o)
{
	const char *value = getenv("GIT_TEST_PARALLEL_CHECKOUT_MODE");

	if (value) {
		if (!strcmp(value, "sync"))
			return PCM__SYNCHRONOUS;
		if (!strcmp(value, "async"))
			return PCM__ASYNCHRONOUS;
		warning("unknown value for GIT_TEST_PARALLEL_CHECKOUT_MODE '%s'",
			value);
		/*fallthru*/
	}

	// TODO
	// TODO do something here...
	// TODO
	return PCM__SYNCHRONOUS;
}

enum parallel_checkout_mode setup_parallel_checkout(
	struct checkout *state,
	struct unpack_trees_options *o)
{
	struct parallel_checkout *pc = NULL;
	enum parallel_checkout_mode pcm = PCM__NONE;
	int nr_updated_files = 0;
	int nr_eligible_files = 0;
	int err;
	int k;

	if (!parallel_checkout_enabled())
		return PCM__NONE;

	/*
	 * Disallow parallel-checkout if this obscure flag is turned on
	 * since it makes the work to be done even more dependent on the
	 * current state of the working directory.
	 */
	if (state->not_new)
		return PCM__NONE;

	/*
	 * Disallow parallel-checkout if we're not actually going to
	 * populate the worktree.
	 */
	if (!o->update || o->dry_run)
		return PCM__NONE;

	/*
	 * See if we have enough CPUs to be effective and choose how many
	 * background checkout--helper processes we should start.
	 *
	 * If the user set a config value, we try to respect it.
	 *
	 * When unspecified, we start with the number of CPUs and adapt
	 * because each helper will have at least 3 threads (main, preload,
	 * and (upto n) writers).  And round down because of the (current)
	 * foreground process.
	 */
	if (core_parallel_checkout_helpers > 0)
		nr_helper_processes_wanted = core_parallel_checkout_helpers;
	else
		nr_helper_processes_wanted = online_cpus() / 3;
	if (nr_helper_processes_wanted < 1)
		return PCM__NONE;

	trace2_region_enter("pcheckout", "setup", NULL);

	/*
	 * First-order approximation of the total amount of work required.
	 *
	 * In this loop, only count the regular files that need updating
	 * (and without regard to eligibility) because this needs to be a
	 * fast threshold scan.
	 */
	for (k = 0; k < state->istate->cache_nr; k++) {
		const struct cache_entry *ce = state->istate->cache[k];

		if (!(ce->ce_flags & CE_UPDATE))
			continue;

		if ((ce->ce_mode & S_IFMT) != S_IFREG)
			continue;

		if (ce->ce_flags & CE_WT_REMOVE)
		    BUG("both update and delete flags are set on %s",
			ce->name);

		nr_updated_files++;
	}
	if (nr_updated_files < parallel_checkout_threshold())
		goto done;

	pc = xcalloc(1, sizeof(*state->parallel_checkout));

	strbuf_init(&pc->base_dir, 0);
	strbuf_add(&pc->base_dir, state->base_dir, state->base_dir_len);

	pcm = compute_best_pcm(state, o);
	pc->pcm = pcm;

	/*
	 * When in synchronous mode, files are not written until explicitly
	 * and individually requested by the client.  So the only goal of
	 * parallel-checkout is to distribute blob preloading across multiple
	 * processes.  So we spread horizontally, so that all helper processes
	 * get some of the first blobs in the index.
	 *
	 * When in asynchronous mode, spread the other way so that the
	 * helper process are touching different parts of the file
	 * system when writing their allotment (in the obscure hope
	 * that that will ease any per-directory lock contention in
	 * the kernel).
	 */
	if (pc->pcm == PCM__SYNCHRONOUS)
		pc->hsm = HSM__HORIZONTAL;
	else
		pc->hsm = HSM__VERTICAL;

	/*
	 * When in synchronous mode, we only need 1 writer because files are
	 * not written until explicitly and individually requested by the
	 * client.  So ignore the config settings.
	 */
	if (pc->pcm == PCM__SYNCHRONOUS)
		nr_writer_threads = 1;
	else {
		nr_writer_threads = core_parallel_checkout_writers;
		if (nr_writer_threads < 1)
			nr_writer_threads = DEFAULT_PARALLEL_CHECKOUT_WRITERS;
	}

	nr_preloads = core_parallel_checkout_preload;
	if (nr_preloads < 1)
		nr_preloads = DEFAULT_PARALLEL_CHECKOUT_PRELOAD;

	/*
	 * Queue the set of ELIGIBLE regular files that need to be updated.
	 *
	 * Use this sequential pass thru the index to capture the smudging
	 * rules required for each file.
	 *
	 * [] The rules for the .gitattributes "attribute stack" make
	 *    it path-relative.  So we evaluate the stack sequentially
	 *    during the index iteration which visits paths in
	 *    depth-first order and naturally fits the stack mode.
	 *
	 * [] This avoids the natural tendency to try to delay the
	 *    evaluation of the attribute stack until we are in a
	 *    parallel context and distribute the work.
	 *
	 *    This is problematic because:
	 *
	 *    [] It would require the attribute stack code to be
	 *       thread-safe and/or introduce lock overhead to that
	 *       computation.
	 *
	 *    [] During the attribute stack evaluation we may need to hit
	 *       the ODB to fetch a not-yet-populated ".gitattributes" file
	 *       somewhere deep in the tree to evaluate a peer item in the
	 *       current directory or in a child directory visited before
	 *       the attributes file is visited during this depth-first
	 *       iteration.  Since we know the ODB is not thread safe, we
	 *       want to avoid that situation.
	 */
	trace2_region_enter("pcheckout", "build_items", NULL);
	for (k = 0; k < state->istate->cache_nr; k++) {
		struct cache_entry *ce = state->istate->cache[k];
		struct parallel_checkout_item *item;
		struct conv_attrs ca;

		if (!(ce->ce_flags & CE_UPDATE))
			continue;
		if ((ce->ce_mode & S_IFMT) != S_IFREG)
			continue;

		convert_attrs(state->istate, &ca, ce->name);
		if (!is_eligible_for_parallel_checkout(&ca))
			continue;

		nr_eligible_files++;

		item = xcalloc(1, sizeof(*item));
		/* item->item_error_class = IEC__NO_RESULT */

		item->ce = ce;
		ce->parallel_checkout_item = item;

		item->ca.drv = ca.drv;
		item->ca.attr_action = ca.attr_action;
		item->ca.crlf_action = ca.crlf_action;
		item->ca.ident = ca.ident;
		if (ca.working_tree_encoding && *ca.working_tree_encoding)
			item->ca.working_tree_encoding =
				strdup(ca.working_tree_encoding);

		item->pc_item_nr = pc->nr; /* our position in the item array */

		item->child_nr = -1; /* not yet assigned to a helper process */
		item->helper_item_nr = -1; /* not yet sent to a helper process */

		ALLOC_GROW(pc->items, pc->nr + 1, pc->alloc);
		pc->items[pc->nr++] = item;
	}
	trace2_region_leave("pcheckout", "build_items", NULL);
	assert(pc->nr == nr_eligible_files);
	if (pc->nr < parallel_checkout_threshold()) {
		free_parallel_checkout(pc);
		goto done;
	}

	if (launch_all_helpers(pc)) {
		free_parallel_checkout(pc);
		stop_all_helpers();
		goto done;
	}

	sigchain_push(SIGPIPE, SIG_IGN);
	err = send_items_to_helpers(pc);
	sigchain_pop(SIGPIPE);

	if (err) {
		free_parallel_checkout(pc);
		stop_all_helpers();
		goto done;
	}

	/*
	 * Actually enable parallel checkout.
	 */
	state->parallel_checkout = pc;
	assert(pc->pcm != PCM__NONE);

done:
	trace2_data_intmax("pcheckout", NULL, "ce/nr_total",
			   state->istate->cache_nr);
	trace2_data_intmax("pcheckout", NULL, "ce/nr_updated",
			   nr_updated_files);
	trace2_data_intmax("pcheckout", NULL, "ce/nr_eligible",
			   nr_eligible_files);
	trace2_data_intmax("pcheckout", NULL, "core/threshold",
			   parallel_checkout_threshold());

	trace2_data_intmax("pcheckout", NULL, "pcm", pcm);
	if (pcm != PCM__NONE) {
		trace2_data_intmax("pcheckout", NULL, "helper/processes",
				   nr_helper_processes_wanted);
		trace2_data_intmax("pcheckout", NULL, "helper/writer_threads",
				   nr_writer_threads);
		trace2_data_intmax("pcheckout", NULL, "helper/preload_count",
				   nr_preloads);
	}

	trace2_region_leave("pcheckout", "setup", NULL);

	return pcm;
}

void finish_parallel_checkout(struct checkout *state)
{
	if (!state->parallel_checkout)
		return;

	trace2_region_enter("pcheckout", "finish", NULL);

	free_parallel_checkout(state->parallel_checkout);
	state->parallel_checkout = NULL;

	trace2_region_leave("pcheckout", "finish", NULL);
}

/*
 * Apply successful response from the helper (and lstat data) to the
 * cache-entry.
 *
 * This should match the code at the bottom of entry.c:write_entry()
 * at the "finish:" label.
 */
static void parallel_checkout__update_cache_entry(
	const struct checkout *state,
	struct cache_entry *ce,
	enum checkout_helper__item_error_class item_error_class,
	struct stat *st)
{
#if 0
#ifdef GIT_WINDOWS_NATIVE
	/* Flush cached lstat in fscache after writing to disk. */
	flush_fscache();
#endif
#endif

	if (state->refresh_cache) {
		assert(state->istate);

		if (item_error_class == IEC__OK) {
			fill_stat_cache_info(state->istate, ce, st);
			ce->ce_flags |= CE_UPDATE_IN_BASE;
			mark_fsmonitor_invalid(state->istate, ce);
			state->istate->cache_changed |= CE_ENTRY_CHANGED;
		}
	}
}

/*
 * Was this item succefully populated by an asynchronous-mode helper?
 *
 * Returns:
 * 1 if the item was successfully populated.
 * 0 if there is a retryable error.
 * -1 if there was an error that should not be retried.
 *
 * Emit an error message for the latter ones.
 */
int parallel_checkout__async__classify_result(const struct checkout *state,
					      struct cache_entry *ce,
					      struct progress *progress,
					      unsigned *result_cnt)
{
	struct parallel_checkout_item *item = ce->parallel_checkout_item;
	unsigned cnt = *result_cnt;
	int result;

	assert(state->parallel_checkout->pcm == PCM__ASYNCHRONOUS);
	assert(item);

	switch (item->item_error_class) {
	case IEC__OK:
		/*
		 * This item was completely handled and progress meter
		 * was advanced.
		 */
		assert(item->progress_claimed);
		assert(!(ce->ce_flags & CE_UPDATE));
		result = 1;

		break;

	case IEC__NO_RESULT:
		/*
		 * The helper died or for some other reason we did not get
		 * a response for this item.  The progress meter was not
		 * advanced.
		 */
		assert(!item->progress_claimed);
		assert(ce->ce_flags & CE_UPDATE);

		result = 0;
		break;

	case IEC__OPEN:
		/*
		 * The helper could not create the file, suppress
		 * the error message and let the classic code try it.
		 *
		 * The progress meter was not advanced.
		 *
		 * (The parallel code doesn't try do the
		 * lstat/is-clean/delete logic -- it only does
		 * open(O_CREAT).)  This has the side-effect of adding
		 * it to the clone collision data if appropriate.)
		 */
		assert(!item->progress_claimed);
		assert(ce->ce_flags & CE_UPDATE);

		result = 0;
		break;

	default:
		/*
		 * For any other helper errors, just print the error
		 * message and go on.
		 *
		 * We only really expect a missing blob or full disk
		 * error and trying it again probably won't fix that.
		 *
		 * Advance the progress meter.
		 */
		assert(!item->progress_claimed);
		assert(ce->ce_flags & CE_UPDATE);
		parallel_checkout__print_helper_error(item);

		item->progress_claimed = 1;
		display_progress(progress, ++cnt);
		ce->ce_flags &= ~CE_UPDATE;

		result = -1;
		break;
	}

	*result_cnt = cnt;

	return result;
}

/*
 * Was this item created by a helper?
 */
int parallel_checkout__created_file(struct cache_entry *ce)
{
	struct parallel_checkout_item *item = ce->parallel_checkout_item;

	assert(item);

	switch (item->item_error_class) {
	case IEC__NO_RESULT: /* we don't know */
		return 0;

	case IEC__INVALID_ITEM: /* item unknown to helper */
		return 0;

	case IEC__OK:
		return 1;

	case IEC__LOAD:
		return 0;

	case IEC__OPEN:
		return 0;

	case IEC__WRITE: /* created but couldn't write (eg. disk space?) */
		return 1;

	case IEC__LSTAT: /* created but couldn't lstat ? */
		return 1;

	default:
		error("Invalid IEC for file '%s': %d",
		      item->ce->name, item->item_error_class);
		return 0;
	}
}

/*
 * Ask the helper associated with this cache-entry to write the
 * smudged content to disk.  Block the current process until it
 * is finished.
 *
 * This is conceptually a peer of entry.c:write_entry() but uses
 * the parallel-checkout helpers to actually populate the worktree.
 * It assumes that entry.c:checkout_entry() has already handled
 * creating and deleting of directories and/or files that would
 * collide with the file we are about to create.
 *
 * This function is much simpler than entry.c:write_entry() because
 * the parallel-eligible code only queues up certain types of
 * regular files and we don't have to worry about symlinks, gitlinks,
 * and etc.
 *
 * We also assume that we are not writing to a temp file and
 * therefore don't need an alternate path.
 *
 * Use this routine when the checkout needs to be very synchronous
 * WRT how/when files are populated in the worktree (such as after
 * sequentially scanning for uncomitted changes before overwriting).
 *
 * Return 1 if we have any errors.
 */
int parallel_checkout__sync__write_entry(const struct checkout *state,
					 struct cache_entry *ce)
{
	struct parallel_checkout_item *item = ce->parallel_checkout_item;
	struct stat st;
	int err = 0;

	assert(item->ce == ce);

	sigchain_push(SIGPIPE, SIG_IGN);
	err = send_cmd__sync_write(item, &st);
	sigchain_pop(SIGPIPE);

	parallel_checkout__update_cache_entry(state, ce,
					      item->item_error_class, &st);
	parallel_checkout__print_helper_error(item);

	err |= item->item_error_class != IEC__OK;

	return err;
}

/*
 * Request progress information and item results from a helper.
 *
 * We receive data for zero or more items.  This is the set of
 * contiguous items (from the point of view of this helper) that
 * are currently marked DONE and haven't been previously reported.
 *
 * Return 1 if we have a packet IO failure.  (Errors for actually
 * populating files are handled later.)
 */
static int get_helper_progress(const struct checkout *state, int child_nr,
			       struct progress *progress, unsigned *result_cnt)
{
	struct parallel_checkout *pc = state->parallel_checkout;
	struct helper_process *hp = helper_pool.array[child_nr];
	struct child_process *process = &hp->subprocess.process;
	struct parallel_checkout_item *item;
	char *line;
	int len;
	struct stat st;
	const struct checkout_helper__item_result *temp;
	unsigned cnt = *result_cnt;

	packet_write_fmt_gently(process->in, "command=%s\n",
				CAP_ASYNC_PROGRESS_NAME);
	if (packet_flush_gently(process->in)) {
		hp->helper_is_dead_to_us = 1;
		return 1;
	}

	while (1) {
		len = packet_read_line_gently(process->out, NULL, &line);
		if (len < 0 || !line)
			break;

		if (len != sizeof(struct checkout_helper__item_result))
			BUG("checkout-helper response wrong (obs %d, exp %d)",
			    len,
			    (int)sizeof(struct checkout_helper__item_result));

		/*
		 * Peek at the response and make sure it looks right before use it.
		 */
		temp = (const struct checkout_helper__item_result *)line;
		assert(temp->item_error_class != IEC__NO_RESULT);
		assert(temp->item_error_class != IEC__INVALID_ITEM);
		assert(temp->helper_item_nr < hp->helper_item_count);
		assert(temp->pc_item_nr < pc->nr);

		/*
		 * Find the corresponding parallel_checkout_item in our vector
		 * for the response and verify that is the one we sent to the
		 * helper.
		 */
		item = pc->items[temp->pc_item_nr];
		assert(item->helper_item_nr == temp->helper_item_nr);
		assert(item->pc_item_nr == temp->pc_item_nr);
		assert(item->child_nr == child_nr);

		item->item_error_class = temp->item_error_class;
		item->item_errno = temp->item_errno;
		memcpy(&st, &temp->st, sizeof(st));

		debug_dump_item(item, "async-progress", &st);

		if (temp->helper_item_nr != hp->helper_result_count)
			BUG("did not receive contiguous, in-order item results");
		hp->helper_result_count++;

		parallel_checkout__update_cache_entry(state, item->ce,
						      item->item_error_class,
						      &st);

		if (item->item_error_class == IEC__OK) {
			/*
			 * Only if everything succeeded do we claim that we've
			 * updated the item and advance the progress meter.
			 * For failures, we let the sequential loop decide whether
			 * to retry and how/when to advance the progress meter.
			 */
			item->progress_claimed = 1;
			display_progress(progress, ++cnt);
			item->ce->ce_flags &= ~CE_UPDATE;
		}
	}

	*result_cnt = cnt;
	return 0;
}

static int child_is_finished(int child_nr)
{
	struct helper_process *hp = helper_pool.array[child_nr];

	if (hp->helper_is_dead_to_us)
		return 1;
	if (hp->helper_result_count == hp->helper_item_count)
		return 1;

	return 0;
}

/*
 * Poll for progress and item results from all of the helpers.
 *
 * This spins until we have results for all items from all helpers.
 *
 * The assumption is that we sent one big batch to each helper at
 * the start because we believe that each helper will probably take
 * about the same amount of time on their portion.  Specifically,
 * our caller is not attempting to chunk smaller batches to each
 * helper in an attempt to send subsequent batches to helpers that
 * finish early.
 *
 * Returns 1 if there was a packet IO failure.
 */
int parallel_checkout__async__progress(const struct checkout *state,
				       struct progress *progress,
				       unsigned *result_cnt)
{
	int child_nr;
	int err = 0;
	int nr_children_still_working = 0;

	assert(is_parallel_checkout_mode(state, PCM__ASYNCHRONOUS));

	trace2_region_enter("pcheckout", "async/progress", NULL);

	sigchain_push(SIGPIPE, SIG_IGN);

	do {
		nr_children_still_working = helper_pool.nr;

		for (child_nr = 0; child_nr < helper_pool.nr; child_nr++) {
			if (child_is_finished(child_nr))
				nr_children_still_working--;
			else
				err |= get_helper_progress(state, child_nr,
							   progress,
							   result_cnt);
		}
	} while (nr_children_still_working);

	sigchain_pop(SIGPIPE);

	trace2_region_leave("pcheckout", "async/progress", NULL);

	/*
	 * The checkout--helpers have completed all of the items
	 * that they have been given.  We currently send everything
	 * in a single batch, so we have no more need for the helper
	 * processes.  Gently tell them to exit while our caller
	 * continues to process the results.
	 */
	stop_all_helpers();

	return err;
}
