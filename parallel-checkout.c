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
		 * not assume that all filters are safe.  For example,
		 * one that needs to access the keychain and/or prompt
		 * the user might get confusing.  So we don't allow
		 * this.
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

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

static int nr_helper_processes_wanted = 1;
static int nr_writer_threads_per_helper_process_wanted = 1;
static int preload_queue_size = 5;

static int helper_pool_initialized;

struct helper_process {
	struct subprocess_entry subprocess; /* must be first */
	unsigned int supported_capabilities;
	int helper_item_count;
	int max_sent_async_write;
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
	struct cache_entry *ce;     /* we do not own this */
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
	int helper_nr;
	/*
	 * The item number that the child process knows it as.  This is a
	 * contiguous series WRT that child.
	 */
	int helper_item_nr;

	/*
	 * A place to receive the results for this item from the child
	 * helper process.
	 */
	struct checkout_helper__item_result item_result;
};

struct parallel_checkout {
	struct parallel_checkout_item **items;
	int nr, alloc;
	struct strbuf base_dir;
};

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

///////////////////////////////////////////////////////////////////

/*
 * Print an error message describing why the helper associated with
 * this item could not create the file.  This lets the foreground
 * process control the ordering of messages to better match the
 * order from the classic (sequential) version.
 */
static int helper_error(struct parallel_checkout_item *item)
{
	switch (item->item_result.item_error_class) {
	case IEC__OK:
		return IEC__OK;

	case IEC__INVALID_ITEM:
		/*
		 * We asked helper[k] for an item that we did not send it.
		 */
		error("Invalid item for helper[%d] '%s'",
		      item->helper_nr, item->ce->name);
		return 1;

	case IEC__LOAD:
		error("error loading blob for '%s': %s",
		      item->ce->name, strerror(item->item_result.item_errno));
		return 1;

	case IEC__OPEN:
		error("error creating file '%s': %s",
		      item->ce->name, strerror(item->item_result.item_errno));
		return 1;

	case IEC__WRITE:
		error("error writing to file '%s': %s",
		      item->ce->name, strerror(item->item_result.item_errno));
		return 1;

	case IEC__LSTAT:
		error("error stating file '%s': %s",
		      item->ce->name, strerror(item->item_result.item_errno));
		return 1;

	default:
		error("Invalid IEC for file '%s': %d",
		      item->ce->name, item->item_result.item_error_class);
		return 1;
	}
}

///////////////////////////////////////////////////////////////////

#define CAP_QUEUE       (1u<<1)
#define CAP_WRITE       (1u<<2)
#define CAP_GET1        (1u<<3)
#define CAP_MGET        (1u<<4)
#define CAP_EVERYTHING  (CAP_QUEUE | CAP_WRITE | CAP_GET1 | CAP_MGET)

#define CAP_QUEUE_NAME  "queue"
#define CAP_WRITE_NAME  "write"
#define CAP_GET1_NAME   "get1"
#define CAP_MGET_NAME   "mget"

static int helper_start_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = {1, 0};
	static struct subprocess_capability capabilities[] = {
		{ CAP_QUEUE_NAME, CAP_QUEUE },
		{ CAP_WRITE_NAME, CAP_WRITE },
		{ CAP_GET1_NAME,  CAP_GET1  },
		{ CAP_MGET_NAME,  CAP_MGET  },
		{ NULL, 0 }
	};

	struct helper_process *hp = (struct helper_process *)subprocess;

	return subprocess_handshake(subprocess, "checkout--helper", versions,
				    NULL, capabilities,
				    &hp->supported_capabilities);
}

/*
 * Start a helper process.  The helper_nr is there to force the subprocess
 * mechanism to let us have more than one instance of the same executable
 * (and to help with tracing).
 *
 * The returned helper_process pointer belongs to the map.
 */
static struct helper_process *helper_find_or_start_process(
	unsigned int cap_needed, int helper_nr)
{
	struct helper_process *hp;
	struct argv_array argv = ARGV_ARRAY_INIT;
	struct strbuf quoted = STRBUF_INIT;

//	argv_array_push(&argv, "/home/jeffhost/work/gfw/git-checkout--helper");
	argv_array_push(&argv, "checkout--helper");
	argv_array_pushf(&argv, "--child=%d", helper_nr);
	argv_array_pushf(&argv, "--writers=%d", nr_writer_threads_per_helper_process_wanted);
	argv_array_pushf(&argv, "--preload=%d", preload_queue_size);

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
//		if (subprocess_start(&helper_pool_map, &hp->subprocess,
//				     quoted.buf, helper_start_fn))
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

static void stop_all_helpers(void)
{
	struct helper_process *hp;
	int k;

	trace2_region_enter("pcheckout", "stop_helpers", NULL);

	for (k = 0; k < helper_pool.nr; k++) {
		hp = helper_pool.array[k];
		subprocess_stop(&helper_pool_map, &hp->subprocess);
		helper_pool.array[k] = NULL;
	}

	trace2_region_leave("pcheckout", "stop_helpers", NULL);

	FREE_AND_NULL(helper_pool.array);
	helper_pool.nr = 0;
	helper_pool.alloc = 0;
}

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

static void send__queue_item_record(struct parallel_checkout *pc,
				    int pc_item_nr, int helper_nr)
{
	struct parallel_checkout_item *item = pc->items[pc_item_nr];
	struct conv_attrs *ca = &item->ca;
	struct helper_process *hp = helper_pool.array[helper_nr];
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
	item->helper_nr = helper_nr;
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

/*
 * "async write" tells helper[helper_nr] that all items in [0, end) can
 * be written when ready.
 */
static int send_cmd__async_write(int helper_nr, int end)
{
	struct helper_process *hp = helper_pool.array[helper_nr];
	struct child_process *process = &hp->subprocess.process;

	/*
	 * We can only extend the range, so don't bother sending
	 * a smaller range that the helper will ignore.
	 */
	if (end <= hp->max_sent_async_write)
		return 0;

	if (packet_write_fmt_gently(process->in, "command=write\n"))
		return 1;
	if (packet_write_fmt_gently(process->in, "end=%d\n", end))
		return 1;
	if (packet_flush_gently(process->in))
		return 1;

	hp->max_sent_async_write = end;

	return 0;
}

/*
 * "async write" tells the helper associated with this item that all
 * items in [0, item_nr] can be written when ready.
 */
static int send_cmd__async_write_item(struct parallel_checkout_item *item)
{
	return send_cmd__async_write(item->helper_nr,
				     item->helper_item_nr + 1);
}

static void debug_dump_item(const struct parallel_checkout_item *item,
			    const char *label)
{
#if 0
	trace2_printf(
		"%s: h[%d] req[%d,%d] res[%d,%d] e[%d,%d] st[%d,%d]",
		label,

		item->helper_nr,

		item->pc_item_nr,
		item->helper_item_nr,

		item->item_result.pc_item_nr,
		item->item_result.helper_item_nr,

		item->item_result.item_error_class,
		item->item_result.item_errno,

		item->item_result.st.st_size,
		item->item_result.st.st_mtime);
#endif
}

static int send_cmd__sync_get1_item(struct parallel_checkout_item *item)
{
	char buffer[LARGE_PACKET_MAX];
	struct helper_process *hp = helper_pool.array[item->helper_nr];
	struct child_process *process = &hp->subprocess.process;
	char *line;
	int len;

	if (packet_write_fmt_gently(process->in, "command=get1\n"))
		return 1;
	if (packet_write_fmt_gently(process->in, "nr=%d\n",
				    item->helper_item_nr))
		return 1;
	if (packet_flush_gently(process->in))
		return 1;

	len = packet_read_line_gently_r(process->out, NULL, &line,
					buffer, sizeof(buffer));
	if (len < 0 || !line)
		BUG("sync_get1: premature flush or EOF");

	if (len != sizeof(struct checkout_helper__item_result))
		BUG("sync_get1: wrong length (obs %d, exp %d)",
		    len, (int)sizeof(struct checkout_helper__item_result));

	memcpy(&item->item_result, line,
	       sizeof(struct checkout_helper__item_result));

	debug_dump_item(item, "sync_get1");

	if (item->item_result.helper_item_nr != item->helper_item_nr)
		BUG("sync_get1: h[%d] wrong item req[%d,%d] rcv[%d,%d]",
		    item->helper_nr,
		    item->pc_item_nr, item->helper_item_nr,
		    item->item_result.pc_item_nr, item->item_result.helper_item_nr);
	if (item->item_result.pc_item_nr != item->pc_item_nr ||
	    item->item_result.item_error_class == IEC__INVALID_ITEM)
		BUG("sync_get1: h[%d] unk item req[%d,%d] rcv[%d,%d]",
		    item->helper_nr,
		    item->pc_item_nr, item->helper_item_nr,
		    item->item_result.pc_item_nr, item->item_result.helper_item_nr);

	/* eat the flush packet */
	while (1) {
		len = packet_read_line_gently_r(process->out, NULL, &line,
						buffer, sizeof(buffer));
		if (len < 0 || !line)
			break;
	}

	return 0;
}

/*
 * blocking mget results for [0, end) from this child.
 */
static int send_cmd__sync_mget_items(struct parallel_checkout *pc,
				     int helper_nr)
{
	char buffer[LARGE_PACKET_MAX];
	struct helper_process *hp = helper_pool.array[helper_nr];
	struct child_process *process = &hp->subprocess.process;
	struct parallel_checkout_item *item;
	char *line;
	int len;
	const struct checkout_helper__item_result *temp;
	int helper_item_nr;

	/*
	 * Currently, we are requesting data for all of the items that
	 * we sent to this helper:  [0, helper_item_count)
	 *
	 * TODO consider using [begin, end) to chunk them back.
	 */

	if (packet_write_fmt_gently(process->in, "command=mget\n"))
		return 1;
	if (packet_write_fmt_gently(process->in, "end=%d\n",
				    hp->helper_item_count))
		return 1;
	if (packet_flush_gently(process->in))
		return 1;

	for (helper_item_nr = 0; 1; helper_item_nr++) {
		len = packet_read_line_gently_r(process->out, NULL, &line,
						buffer, sizeof(buffer));
		if (len < 0 || !line)
			break;

		if (helper_item_nr >= hp->helper_item_count)
			BUG("sync_mget: h[%d] too many rows (obs %d, exp %d)",
			    helper_nr,
			    helper_item_nr, hp->helper_item_count);

		if (len != sizeof(struct checkout_helper__item_result))
			BUG("sync_mget: wrong length (obs %d, exp %d)",
			    len, (int)sizeof(struct checkout_helper__item_result));

		/*
		 * Peek at the response and make sure it looks right before use it.
		 */
		temp = (const struct checkout_helper__item_result *)line;
		assert(temp->helper_item_nr == helper_item_nr);
		assert(temp->item_error_class != IEC__INVALID_ITEM);
		assert(temp->pc_item_nr < pc->nr);

		/*
		 * Map this result back to the associated item in item_vec[]
		 * without assuming any knowledge of the distribution spread.
		 */
		item = pc->items[temp->pc_item_nr];

		/*
		 * Verify that this item from item_vec[] was in fact sent to
		 * this helper with this helper_item_nr.
		 */
		assert(item->pc_item_nr == temp->pc_item_nr);
		assert(item->helper_item_nr == temp->helper_item_nr);
		assert(item->helper_nr == helper_nr);

		memcpy(&item->item_result, line,
		       sizeof(struct checkout_helper__item_result));

		debug_dump_item(item, "sync_mget");

		/*
		 * Just store the results from the helper for now
		 * within the item.  Defer updating the cache-entry
		 * until all results have been received -- in case we
		 * have to arbitrate if there are collisions, for
		 * example.
		 */
	}

	return 0;
}

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

/*
 * Spread the items "horizontally" across all of the helpers
 * "horizontally" so that they will each have some of the first blobs
 * that we need (during our sequential iteration thru the index).
 *
 * Use a simple `j mod nr_helpers` method to assign items to
 * helper processes.  This blindly spreads the work uniformly across
 * the pool without regard to blob size or ODB-storage location.
 *
 * TODO A future effort might try to distribute by blob size, by
 * packfile locality, or some other criteria if we find that the
 * foreground process (when running in "sync mode") is waiting for
 * blobs during its in-order traversal of the index to populate the
 * work tree.
 *
 * To helper[k] we send:
 *     command=queue<LF>
 *     <binary data for item>
 *     <binary data for item>
 *     ...
 *     <flush>
 *
 * Where each record looks like:
 *     <fixed-fields>[<encoding>]<path>
 *
 * If we get any errors while seeding the helpers with work, just stop
 * the parallel checkout and let the caller do a normal sequential
 * checkout.  Afterall, none of the children have actually done
 * anything to the worktree yet.
 */
static int send_items_to_helpers(struct parallel_checkout *pc)
{
	int helper_nr;
	int pc_item_nr;
	int err = 0;

	trace2_region_enter("pcheckout", "send_items", NULL);

	/*
	 * Begin a queue command with each helper in parallel.
	 */
	for (helper_nr = 0; helper_nr < helper_pool.nr; helper_nr++) {
		struct helper_process *hp = helper_pool.array[helper_nr];
		struct child_process *process = &hp->subprocess.process;

		if (packet_write_fmt_gently(process->in, "command=queue\n")) {
			err = 1;
			goto done;
		}
	}

	/*
	 * Distribute the array of items to the helpers as part of
	 * the queue command that we've opened.
	 */
	for (pc_item_nr = 0; pc_item_nr < pc->nr; pc_item_nr++) {
		int helper_nr = pc_item_nr % helper_pool.nr;

		send__queue_item_record(pc, pc_item_nr, helper_nr);
	}

	/*
	 * close the queue command with each helper.
	 */
	for (helper_nr = 0; helper_nr < helper_pool.nr; helper_nr++) {
		struct helper_process *hp = helper_pool.array[helper_nr];
		struct child_process *process = &hp->subprocess.process;

		if (packet_flush_gently(process->in)) {
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

	// TODO Revisit how to set these knobs.
	// TODO
	// TODO Maybe be config settings and maybe depends upon the
	// TODO command (clone vs switch-branch).
	// TODO
	nr_helper_processes_wanted = 6;
	nr_writer_threads_per_helper_process_wanted = online_cpus() / 4;
	preload_queue_size = (nr_writer_threads_per_helper_process_wanted * 2) + 10;
	
	trace2_region_enter("pcheckout", "launch_helpers", NULL);

	ALLOC_GROW(helper_pool.array, nr_helper_processes_wanted, helper_pool.alloc);

	while (helper_pool.nr < nr_helper_processes_wanted) {
		hp = helper_find_or_start_process(CAP_EVERYTHING, helper_pool.nr);
		if (!hp) {
			err = 1;
			break;
		}

		helper_pool.array[helper_pool.nr++] = hp;
	}

	trace2_region_leave("pcheckout", "launch_helpers", NULL);

	return err;
}

void setup_parallel_checkout(struct checkout *state,
			     struct unpack_trees_options *o)
{
	struct parallel_checkout *pc = NULL;
	int nr_updated_files = 0;
	int nr_eligible_files = 0;
	int enabled;
	int err;
	int k;

	if (!core_parallel_checkout)
		return;

	/*
	 * Disallow parallel-checkout if this obscure flag is turned on
	 * since it makes the work to be done even more dependent on the
	 * current state of the working directory.
	 */
	if (state->not_new)
		return;

	/*
	 * Disallow parallel-checkout if we're not actually going to
	 * populate the worktree.
	 */
	if (!o->update || o->dry_run)
		return;

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
		nr_updated_files++;
	}
	if (nr_updated_files <= core_parallel_checkout_threshold)
		goto done;

	pc = xcalloc(1, sizeof(*state->parallel_checkout));

	strbuf_init(&pc->base_dir, 0);
	strbuf_add(&pc->base_dir, state->base_dir, state->base_dir_len);

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

		item->helper_nr = -1; /* not yet assigned to a helper process */
		item->helper_item_nr = -1; /* not yet sent to a helper process */

		ALLOC_GROW(pc->items, pc->nr + 1, pc->alloc);
		pc->items[pc->nr++] = item;

//		trace2_printf("setup_parallel_checkout: ce[%d] item[%d] '%s'",
//			      k, item->pc_item_nr, ce->name);
	}
	trace2_region_leave("pcheckout", "build_items", NULL);
	assert(pc->nr == nr_eligible_files);
	if (pc->nr <= core_parallel_checkout_threshold) {
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
	enabled = 1;

done:
	trace2_data_intmax("pcheckout", NULL, "ce/nr_total",
			   state->istate->cache_nr);
	trace2_data_intmax("pcheckout", NULL, "ce/nr_updated",
			   nr_updated_files);
	trace2_data_intmax("pcheckout", NULL, "ce/nr_eligible",
			   nr_eligible_files);
	trace2_data_intmax("pcheckout", NULL, "core/threshold",
			   core_parallel_checkout_threshold);

	trace2_data_intmax("pcheckout", NULL, "helper/enabled", enabled);
	if (enabled) {
		trace2_data_intmax("pcheckout", NULL, "helper/processes",
				   nr_helper_processes_wanted);
		trace2_data_intmax("pcheckout", NULL, "helper/writer_threads",
				   nr_writer_threads_per_helper_process_wanted);
		trace2_data_intmax("pcheckout", NULL, "helper/preload_count",
				   preload_queue_size);
	}

	trace2_region_leave("pcheckout", "setup", NULL);
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

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

/*
 * Apply response from the helper (and lstat data) to the cache-entry.
 *
 * This should match the code at the bottom of entry.c:write_entry()
 * at the "finish:" label.
 */
static int update_cache_entry_for_item(const struct checkout *state,
				       struct parallel_checkout_item *item)
{
	assert(state->istate);
	assert(state->refresh_cache);

	if (item->item_result.item_error_class != IEC__OK) {
		helper_error(item);
		return 1;
	}

	// TODO flush_fscache() ??

	fill_stat_cache_info(state->istate, item->ce,
			     &item->item_result.st);
	item->ce->ce_flags |= CE_UPDATE_IN_BASE;
	mark_fsmonitor_invalid(state->istate, item->ce);
	state->istate->cache_changed |= CE_ENTRY_CHANGED;

	return 0;
}

static int write_entry_1(struct parallel_checkout_item *item)
{
	if (send_cmd__async_write_item(item))
		return 1;
	if (send_cmd__sync_get1_item(item))
		return 1;
	return 0;
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
 */
int parallel_checkout__write_entry(const struct checkout *state,
				   struct cache_entry *ce)
{
	struct parallel_checkout_item *item = ce->parallel_checkout_item;
	int err = 0;

	assert(item->ce == ce);

	sigchain_push(SIGPIPE, SIG_IGN);
	err = write_entry_1(item);
	sigchain_pop(SIGPIPE);

	if (err) {
		/*
		 * We have a pkt-line error trying to talk to the helper
		 * process.  This is probably fatal (at least WRT to this
		 * helper instance).
		 *
		 * The original check_update_loop() just sets an error flag
		 * and keeps going when it gets a file IO error.
		 *
		 * So for now, just emit an error() for it and continue
		 * rather than shutting down.
		 */
		error("TODO could not get result from helper....");
		return err;
	}

	// TODO can we just assert(state->refresh_cache) ?

	if (state->refresh_cache)
		return update_cache_entry_for_item(state, item);
	return 0;
}

#if 0 // temporarily hide this while we work on classic_with_helper mode
int parallel_checkout__set_auto_write(const struct checkout *state)
{
	int helper_nr;
	int err = 0;

	trace2_region_enter("pcheckout", "set_auto_write", NULL);

	sigchain_push(SIGPIPE, SIG_IGN);
	for (helper_nr = 0; helper_nr < helper_pool.nr; helper_nr++)
		err |= send_cmd__async_write(helper_nr,
					     CHECKOUT_HELPER__AUTO_ASYNC_WRITE);
	sigchain_pop(SIGPIPE);

	trace2_region_leave("pcheckout", "set_auto_write", NULL);

	return err;

}

int parallel_checkout__collect_results(const struct checkout *state)
{
	struct parallel_checkout *pc = state->parallel_checkout;
	struct parallel_checkout_item *item;
	int helper_nr;
	int pc_item_nr;
	int err = 0;

	if (!pc)
		return 0;

	trace2_region_enter("pcheckout", "collect_results", NULL);

	sigchain_push(SIGPIPE, SIG_IGN);
	for (helper_nr = 0; helper_nr < helper_pool.nr; helper_nr++) {
		err |= send_cmd__sync_mget_items(pc, helper_nr);
	}
	sigchain_pop(SIGPIPE);

	if (state->refresh_cache) {
		for (pc_item_nr = 0;
		     pc_item_nr < pc->nr;
		     pc_item_nr++) {
			item = pc->items[pc_item_nr];
			err |= update_cache_entry_for_item(state, item);
		}
	}

	trace2_region_leave("pcheckout", "collect_results", NULL);

	// TODO if err handle both client/helper IO errors and
	// TODO update ce problems.

	return err;
}

#endif // temporary
