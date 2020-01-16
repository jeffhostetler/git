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
#include "thread-utils.h"

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

static int helper_pool_initialized;

struct helper_process {
	struct subprocess_entry subprocess; /* must be first */
	unsigned int supported_capabilities;
};

struct helper_pool {
	struct helper_process **array; /* we do not own this */
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
	int item_nr;
	int child_nr;
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

#define CAP_ITEM        (1u<<1)
#define CAP_WRITE       (1u<<2)
#define CAP_ITEM_NAME   "item"
#define CAP_WRITE_NAME  "write"

static int helper_start_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = {1, 0};
	static struct subprocess_capability capabilities[] = {
		{ CAP_ITEM_NAME, CAP_ITEM },
		{ CAP_WRITE_NAME, CAP_WRITE },
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
	unsigned int cap_needed, int child_nr)
{
	struct helper_process *hp;
	struct argv_array argv = ARGV_ARRAY_INIT;
	struct strbuf quoted = STRBUF_INIT;

//	argv_array_push(&argv, "/home/jeffhost/work/gfw/git-checkout--helper");
	argv_array_push(&argv, "checkout--helper");
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
		hp = xmalloc(sizeof(*hp));
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

/*
 * Send an "item" message to one of the helper processes.
 *
 * This message includes the OID and details from the .gitattributes
 * so that the helper can asynchronously load the blob into memory and
 * possibly smudge it into a buffer and have it ready for immediate
 * access later when the foreground checkout needs it.  The assumption
 * is that ODB access is not thread safe and that the packfile lookup,
 * de-delta, and unzip computations are very expensive.
 *
 *     command=item LF 
 *     id=<item_nr> LF
 *     oid=<hex-oid> LF
 *     ...TBC...
 *     <flush>
 *
 * We don't need to wait for a response at this point.  We're just
 * queueing them.  That is, we assume that the child has enough memory
 * to allocate simple structure to hold the fields we're passing.
 */
static int helper_send_1_item(struct parallel_checkout *pc, int item_nr,
			      int child_nr)
{
	struct parallel_checkout_item *item = pc->items[item_nr];
	struct conv_attrs *ca = &item->ca;
	struct helper_process *hp = helper_pool.array[child_nr];
	struct child_process *process = &hp->subprocess.process;

	assert(item_nr == item->item_nr);

	/*
	 * Update the item to remember with whom we queued it.
	 */
	item->child_nr = child_nr;

	if (packet_write_fmt_gently(process->in, "command=item\n"))
		return 1;

	if (packet_write_fmt_gently(process->in, "nr=%d\n", item_nr))
		return 1;

	if (packet_write_fmt_gently(process->in, "oid=%s\n",
				    oid_to_hex(&item->ce->oid)))
		return 1;

	if (packet_write_fmt_gently(process->in, "attr=%d\n",
				    ca->attr_action))
		return 1;
	if (packet_write_fmt_gently(process->in, "crlf=%d\n",
				    ca->crlf_action))
		return 1;
	if (ca->ident)
		if (packet_write_fmt_gently(process->in, "ident=%d\n",
					    ca->ident))
			return 1;
	if (ca->working_tree_encoding && *ca->working_tree_encoding)
		if (packet_write_fmt_gently(process->in, "encoding=%s\n",
					    ca->working_tree_encoding))
			return 1;

	if (packet_write_fmt_gently(process->in, "path=%s%s\n",
				    pc->base_dir.buf, item->ce->name))
		return 1;
	if (packet_write_fmt_gently(process->in, "mode=%o\n",
				    item->ce->ce_mode))
		return 1;

	/*
	 * As described in is_eligible_for_parallel_checkout(), we
	 * don't consider cache-entries that need an external tool to
	 * be eligible.  So they should always have a null driver.  If
	 * we decide to change this, we would also need to send the
	 * driver fields to the helper.
	 */
	if (ca->drv)
		BUG("ineligible cache-entry '%s'", item->ce->name);

	return packet_flush_gently(process->in);
}

/*
 * Spread all of the items across all of the helpers.  Spread them
 * "horizontally" so that they will each have some of the first blobs
 * that we need (during our sequential iteration thru the index).
 *
 * If we get any errors while seeding the helpers with work, just stop
 * the parallel checkout and let the caller do a normal sequential
 * checkout.  Afterall, none of the children have actually done
 * anything to the worktree yet.
 *
 * Use a simple `item_nr mod nr_helpers` method to assign items to
 * helper processes.  This blindly spreads the work uniformly across
 * the pool without regard to blob size or ODB-storage location.
 * A future effort might try to distribute by blob size, by packfile
 * locality, and etc. if we find that the foreground process is
 * waiting for blobs during its in-order traversal of the index to
 * populate the work tree.
 */
static int send_items_to_helpers(struct parallel_checkout *pc)
{
	int item_nr;
	int err = 0;

	trace2_region_enter("pcheckout", "send_items", NULL);
	trace2_data_intmax("pcheckout", NULL, "items", pc->nr);

	for (item_nr = 0; item_nr < pc->nr; item_nr++) {
		int child_nr = item_nr % helper_pool.nr;

		sigchain_push(SIGPIPE, SIG_IGN);
		err = helper_send_1_item(pc, item_nr, child_nr);
		sigchain_pop(SIGPIPE);

		if (err)
			break;
	}

	trace2_region_leave("pcheckout", "send_items", NULL);

	return err;
}

static int launch_all_helpers(struct parallel_checkout *pc)
{
	struct helper_process *hp;
	int nr_helpers_wanted = online_cpus() - 1;
//	int nr_helpers_wanted = online_cpus() / 2;

	trace2_region_enter("pcheckout", "launch_helpers", NULL);
	trace2_data_intmax("pcheckout", NULL, "helpers", nr_helpers_wanted);

	ALLOC_GROW(helper_pool.array, nr_helpers_wanted, helper_pool.alloc);

	while (helper_pool.nr < nr_helpers_wanted) {
		hp = helper_find_or_start_process(CAP_ITEM, helper_pool.nr);
		if (!hp)
			return 1;

		helper_pool.array[helper_pool.nr++] = hp;
	}

	trace2_region_leave("pcheckout", "launch_helpers", NULL);

	return 0;
}


// TODO If any eligible entry has ca->ident set, then we need to lookup
// TODO the ID (for the commit) and send that to the helper processes
// TODO along with the item data and/or in a special header message (command).

void setup_parallel_checkout(struct checkout *state)
{
	struct parallel_checkout *pc = NULL;
	int nr_files = 0;
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
		nr_files++;
	}
	if (nr_files <= core_parallel_checkout_threshold)
		goto done;

	pc = xcalloc(1, sizeof(*state->parallel_checkout));

	strbuf_init(&pc->base_dir, 0);
	strbuf_add(&pc->base_dir, state->base_dir, state->base_dir_len);

	/*
	 * Queue the set of ELIGIBLE regular files that need to be updated.
	 *
	 * Use this sequential pass thru the index to capture the smudging
	 * rules required for each file because:
	 *
	 * [] The rules for the .gitattributes "attribute stack" make
	 *    it path-relative.  So we evaluate the stack sequentially
	 *    during the index iteration (which visits paths in
	 *    depth-first order).  (This avoids the natural tendency
	 *    to delay the evaluation of the attribute stack until we
	 *    are in a parallel context and assuming that the
	 *    attribute stack code is thread safe and requiring lots
	 *    of locking.
	 *
	 * [] Furthermore, during the attribute stack evaluation we
	 *    may need to hit the ODB to fetch a not-yet-populated
	 *    .gitattributes file somewhere deep in the tree.  Since
	 *    we know the ODB is not thread safe, we want to avoid the
	 *    situation.
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

		item->item_nr = pc->nr; /* our position in the item array */

		item->child_nr = -1; /* not yet assigned to a helper process */

		ALLOC_GROW(pc->items, pc->nr + 1, pc->alloc);
		pc->items[pc->nr++] = item;

//		trace2_printf("setup_parallel_checkout: ce[%d] item[%d] '%s'",
//			      k, item->item_nr, ce->name);
	}
	trace2_region_leave("pcheckout", "build_items", NULL);
	if (pc->nr <= core_parallel_checkout_threshold) {
		free_parallel_checkout(pc);
		goto done;
	}

	if (launch_all_helpers(pc)) {
		free_parallel_checkout(pc);
		stop_all_helpers();
		goto done;
	}

	if (send_items_to_helpers(pc)) {
		free_parallel_checkout(pc);
		stop_all_helpers();
		goto done;
	}

	/*
	 * Actually enable parallel checkout.
	 */
	state->parallel_checkout = pc;

done:
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

static int helper_send_write_command(struct parallel_checkout_item *item)
{
	struct helper_process *hp = helper_pool.array[item->child_nr];
	struct child_process *process = &hp->subprocess.process;

	if (packet_write_fmt_gently(process->in, "command=write\n"))
		return 1;
	if (packet_write_fmt_gently(process->in, "nr=%d\n", item->item_nr))
		return 1;
	if (packet_flush_gently(process->in))
		return 1;

	return 0;
}

static int helper_rcv_write_response(struct parallel_checkout_item *item)
{
	char buffer[LARGE_PACKET_MAX];
	struct helper_process *hp = helper_pool.array[item->child_nr];
	struct child_process *process = &hp->subprocess.process;
	char *line;
	const char *token;
	int len;
	int rcv_item_nr = -1;
	int rcv_pccr = -1;
	int rcv_errno = -1;

	while (1) {
		len = packet_read_line_gently_r(process->out, NULL, &line,
						buffer, sizeof(buffer));
		if (len < 0 || !line)
			break;

		if (skip_prefix(line, "write=", &token)) {
			rcv_item_nr = strtol(token, NULL, 10);
			continue;
		}

		if (skip_prefix(line, "pccr=", &token)) {
			rcv_pccr = strtol(token, NULL, 10);
			continue;
		}

		if (skip_prefix(line, "errno=", &token)) {
			rcv_errno = strtol(token, NULL, 10);
			continue;
		}

		BUG("helper_rcv_write_response: unexpected response '%s' "
		    "from child %d on '%s'",
		    line, item->child_nr, item->ce->name);
	}

	if (rcv_item_nr != item->item_nr)
		BUG("helper_rcv_write_response: asked for '%d' received '%d'",
		    item->item_nr, rcv_item_nr);

	/* TODO decide what to do with the received errno and PCCR. */

	return (rcv_pccr != PCCR_OK) || (rcv_errno > 0);
}

/*
 * Ask the helper associated with this cache-entry to write the
 * smudged content to disk.  Block the current process until it
 * is finished.  Collect the mtime data as if we wrote it directly.
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
 * don't need an alternate path.
 */
int parallel_checkout_write_entry(const struct checkout *state,
				  struct cache_entry *ce)
{
	struct parallel_checkout_item *item = ce->parallel_checkout_item;

	assert(item->ce == ce);

	if (helper_send_write_command(item))
		goto failed;
	if (helper_rcv_write_response(item))
		goto failed;

	if (state->refresh_cache) {
		struct stat st;

		assert(state->istate);

		if (lstat(ce->name, &st) < 0)
			return error_errno("unable to stat just-written file %s",
					   ce->name);
		fill_stat_cache_info(state->istate, ce, &st);
		ce->ce_flags |= CE_UPDATE_IN_BASE;
		mark_fsmonitor_invalid(state->istate, ce);
		state->istate->cache_changed |= CE_ENTRY_CHANGED;
	}

	return 0;

failed:
	/* TODO handle write failures */
	return 1;
}
