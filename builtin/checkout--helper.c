#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "pkt-line.h"
#include "trace2.h"
#include "thread-utils.h"
#include "parse-options.h"
#include "object-store.h"
#include "checkout--helper.h"

/*
 * This is used as a label prefix in all error messages
 * and in any trace2 messages to identify this child
 * process.
 */
static char t2_category_name[100];
static int child_nr = -1;

enum item_state {
	ITEM_STATE__NEW = 0,
	ITEM_STATE__QUEUED,
	ITEM_STATE__LOADING,
	ITEM_STATE__LOADED,
	ITEM_STATE__WRITING,
	ITEM_STATE__DONE,
};

struct item {
	enum item_state item_state;
	enum checkout_helper__item_error_class item_error_class;
	int item_errno;

	/* These fields are specified by the client. */
	int pc_item_nr;
	int helper_item_nr;
	struct object_id oid;
	struct conv_attrs ca;
	char *path;
	int mode;

	/* These fields are computed as we load and write the item. */
	unsigned long content_size;
	void *content;
	struct stat st;
};

/*
 * When (count > 0), this defines a range of items of the form:
 *     [begin, end)
 * where:
 *     begin = (end - count)
 *
 * When (count == 0), this defines an empty range.
 */
struct item_range {
	int count;
	int end;
};

struct item_vec {
	struct item **array;
	int nr, alloc;
};

static struct item_vec item_vec;
static struct item_range preload_range;

/*
 * A count of the number of completed items.  This will equal
 * item_vec.nr once all items have been written.
 *
 * This is updated as each writer thread finishes writing an item,
 * so it will eventually reach item_vec.nr, but may differ temporarily
 * when threads finish writing items out of order.
 */
static int completed_count;

/*
 * We always start writing the items in the order queued and expect
 * the client to request them in that order.  Even when we have multiple
 * writers, the items are pulled from the queue in that order.
 *
 * Therefore, we only keep track of the number of items authorized for
 * writing and assume that everything in [0, end) is authorized.
 *
 * We allow `end == maxint` (or any value larger than the number of
 * queued items) to mean fully automatic async write.
 *
 * See CHECKOUT_HELPER__AUTO_WRITE.
 */
static int authorized_end;

static pthread_mutex_t main_mutex;
static pthread_t preload_thread;
static pthread_t *writer_thread_pool;

static int nr_active_writers;

static pthread_cond_t preload_cond;
static pthread_cond_t writer_cond;
static pthread_cond_t done_cond;

static int in_shutdown;

/*
 * If we have spare cycles and plenty of memory, we should be able to
 * keep n items (blob contents) in memory at all times.  This value
 * was chosen at random.  It only needs to be large enough to keep the
 * writer thread(s) from stalling while waiting for the preload thread
 * to load a blob from the ODB.
 *
 * TODO Consider making this more flexible, such as being based on sum
 * of the content sizes of all currently loaded blobs.  Or make it a
 * function of the number of writer threads.
 */
#define DEFAULT_PRELOAD_RANGE_LIMIT 5

static int preload_range_limit = DEFAULT_PRELOAD_RANGE_LIMIT;

/*
 * Number of writer threads in the thread pool.
 *
 * The size of the thread pool is a function of what type of checkout
 * being attempted.  For example, a clone into an empty worktree should
 * be able to write n files in parallel without worrying about stepping
 * on uncommitted changes; whereas a branch switch might need to be more
 * careful and populate files sequentially.
 */
#define DEFAULT_WRITER_THREAD_POOL_SIZE 1

static int writer_thread_pool_size = DEFAULT_WRITER_THREAD_POOL_SIZE;

static struct item *alloc_item(int pc_item_nr, int helper_item_nr, int mode,
			       int attr, int crlf, int ident,
			       const struct object_id *oid,
			       char *encoding, char *path)
{
	struct item *item = xcalloc(1, sizeof(*item));

	item->item_state = ITEM_STATE__NEW;
	item->item_error_class = IEC__OK;
	item->item_errno = 0;

	item->pc_item_nr = pc_item_nr;
	item->helper_item_nr = helper_item_nr;
	item->mode = mode;
	item->ca.attr_action = attr;
	item->ca.crlf_action = crlf;
	item->ca.ident = ident;
	oidcpy(&item->oid, oid);
	item->ca.working_tree_encoding = encoding;
	item->path = path;

#if 1
	trace2_printf("%s: item (%d,%d) %s {%d, %d, %d, %s} {%s, %o}",
		      t2_category_name,
		      pc_item_nr, helper_item_nr,
		      oid_to_hex(oid),
		      attr, crlf, ident,
		      encoding ? encoding : "(nul)",
		      path,
		      mode);
#endif

	return item;
}

static void free_item(struct item *item)
{
	if (!item)
		return;

	free((char *)item->ca.working_tree_encoding);
	free(item->path);
	free(item->content);
	free(item);
}

static void free_item_vec(void)
{
	/* ASSUME ON MAIN THREAD */

	int k;

	assert(in_shutdown); /* so no locking required */

	for (k = 0; k < item_vec.nr; k++)
		free_item(item_vec.array[k]);

	FREE_AND_NULL(item_vec.array);
	item_vec.nr = 0;
	item_vec.alloc = 0;
}

static void item_vec_append(struct item *item)
{
	/* ASSUME ON MAIN THREAD */

	pthread_mutex_lock(&main_mutex);

	/*
	 * As a sanity check, we require the client send us an
	 * helper_item_nr for each item.  This value will later be
	 * used by the client to receive status for the item.  To
	 * avoid building yet another fancy/expensive lookup table, we
	 * require this to be a simple integer matching the item's row
	 * number in our vector.
	 */
	if (item->helper_item_nr != item_vec.nr)
		BUG("invalid helper_item_nr (%d (exp %d)) for '%s'",
		    item->helper_item_nr, item_vec.nr, item->path);

	ALLOC_GROW(item_vec.array, item_vec.nr + 1, item_vec.alloc);
	item_vec.array[item_vec.nr++] = item;

	item->item_state = ITEM_STATE__QUEUED;

	/*
	 * We have a new item in the item queue, but only bother to wake
	 * the preload thread if it hasn't filled its quota yet.
	 */
	if (preload_range.count < preload_range_limit)
	    pthread_cond_signal(&preload_cond);

	pthread_mutex_unlock(&main_mutex);
}

/*
 * Mark all items [0, end) as being eligible for async-write.
 * Remember we always process items in queue order, both when
 * loading and when writing, so we don't need to explicitly mark
 * individual items.
 */
static void set_async_write_on_items(int end)
{
	/* ASSUME ON MAIN THREAD */

//	trace2_printf("%s: set-async-write[%d]", t2_category_name, end);

	pthread_mutex_lock(&main_mutex);

	/*
	 * We can only increase the range (because the writer threads
	 * may already be working items beyond the new proposed lower
	 * limit, so disregard any attempts to narrow the range.
	 *
	 * Similarly, we can't turn off automatic once enabled.
	 *
	 * Then signal one or more writer threads that the set of
	 * items available for writing has increased and that there
	 * may be work to do.  Broadcast to all writer threads (rather
	 * than signaling a single writer thread) when we increase it
	 * by more than 1 item.
	 */
	if (authorized_end == CHECKOUT_HELPER__AUTO_WRITE)
		;

	else if (end == CHECKOUT_HELPER__AUTO_WRITE) {
		authorized_end = CHECKOUT_HELPER__AUTO_WRITE;
		pthread_cond_broadcast(&writer_cond);
	}

	else if (end > authorized_end + 1) {
		authorized_end = end;
		pthread_cond_broadcast(&writer_cond);
	}

	else if (end == authorized_end + 1) {
		authorized_end = end;
		pthread_cond_signal(&writer_cond);
	}

	pthread_mutex_unlock(&main_mutex);
}

/*
 * Wait for preload and writer threads to finish with this item.
 *
 * Since we are in the main thread, we do not look at "in_shutdown"
 * because only the main thread can signal shutdown.
 *
 * Nor do we need to allow for new items to be queued while we wait
 * because only the main thread can append items to the queue.
 *
 * Return the item when it is done.
 * Return NULL if the item does not exist.
 */
static struct item *wait_for_done_item(int helper_item_nr)
{
	/* ASSUME ON MAIN THREAD */

	struct item *item;

	pthread_mutex_lock(&main_mutex);
	if (helper_item_nr >= item_vec.nr) {
		pthread_mutex_unlock(&main_mutex);
		return NULL;
	}

	item = item_vec.array[helper_item_nr];
	while (item->item_state != ITEM_STATE__DONE)
		pthread_cond_wait(&done_cond, &main_mutex);

	pthread_mutex_unlock(&main_mutex);

	return item;
}

#if 0  // not sure i want this
/*
 * Wait for all items in [begin, end) to be completed.
 */
static void wait_for_done_items(int begin, int end)
{
	int k;

	/* ASSUME ON MAIN THREAD */

	pthread_mutex_lock(&main_mutex);

	for (k = begin; k < end; k++) {
		if (k >= item_vec.nr)
			break;

		while (item_vec.array[k]->item_state != ITEM_STATE__DONE)
			pthread_cond_wait(&done_cond, &main_mutex);
	}

	pthread_mutex_unlock(&main_mutex);
}
#endif

/*
 * Load the contents of a blob into memory in the preload background
 * thread.  Fill in the content and length, but otherwise do not alter
 * the item state.
 */
static enum checkout_helper__item_error_class preload_get_item(struct item *item)
{
	/* ASSUME ON PRELOAD THREAD */
	/* ASSERT NO LOCK HELD */

	struct object_info oi = OBJECT_INFO_INIT;
	enum object_type type;

//	trace2_printf("%s:[item %d] loading '%s'", t2_category_name,
//		      item->helper_item_nr, item->path);

	oi.typep = &type;
	oi.sizep = &item->content_size;
	oi.contentp = &item->content;

	if (oid_object_info_extended(the_repository, &item->oid, &oi, 0) < 0)
		return IEC__LOAD;

//	trace2_printf("%s:[item %d] loaded size %d", t2_category_name,
//		      item->helper_item_nr, item->content_size);

	return IEC__OK;
}

struct my_create_file_data {
	struct item *item;
	int fd;
};

static int my_create_file_cb(const char *path, void *cb)
{
	struct my_create_file_data *d = (struct my_create_file_data *)cb;
	unsigned int mode = d->item->mode & 0100 ? 0777 : 0666;

	d->fd = open(d->item->path, O_WRONLY | O_CREAT | O_EXCL, mode);

	return d->fd < 0;
}

static enum checkout_helper__item_error_class smudge_and_write(
	int fd,
	struct item *item)
{
	enum checkout_helper__item_error_class iec = IEC__OK;
	const char *src = item->content;
	unsigned long size = item->content_size;
	struct strbuf nbuf = STRBUF_INIT;
	enum conv_attrs_classification c = classify_conv_attrs(&item->ca);

	/*
	 * Note [1]:
	 * In `item->path` we have the composed pathname (base_dir +
	 * ce->name) and use it to populate the file.  However, the
	 * code in `apply_filter` and `convert_to_working_tree` assume
	 * they have just the ce->name.  But the smudge code only uses
	 * `apply_filter` when a filter or process driver is defined.
	 * Since those files are not "eligible", we do not have to care
	 * about the distinction between `item->path` and `ce->name`.
	 */
	assert(!item->ca.drv);
	assert(c != CA_CLASS_INCORE_FILTER);
	assert(c != CA_CLASS_INCORE_PROCESS);

	if (c == CA_CLASS_STREAMABLE) {
		/*
		 * TODO Currently, we always use the incore model
		 * `convert_to_working_tree_ca` and do not attempt
		 * to stream it using `get_stream_filter_ca`.
		 *
		 * The point of `checkout--helper` is to preload the
		 * blob content into memory (on another core while the
		 * foreground process is busy with business logic that
		 * must be sequential) and have it ready for immediate
		 * writing when the foreground process is ready for
		 * it.
		 *
		 * So the preload thread always does an ordinary
		 * object read (de-delta and unzip) (and on a single
		 * thread) into a buffer.  And a (multi-threaded)
		 * writer thread always does the in-core smudge and
		 * write.
		 *
		 * Perhaps we can relax this for loose objects.
		 */
	}

	/*
	 * This returns 1 if the smudging actually did something and
	 * gave us a new buffer.  Otherwise, the source buffer should
	 * be used as is.
	 */
	if (convert_to_working_tree_ca(&item->ca,
				       item->path, /* See [1] */
				       src, size, &nbuf)) {
		trace2_printf("%s:[item %d] smudged (%d-->%d) '%s'",
			      t2_category_name, item->helper_item_nr,
			      (int)size, (int)nbuf.len, item->path);

		size = nbuf.len;
		src = nbuf.buf;
	}

	if (write_in_full(fd, src, size) < 0) {
		item->item_errno = errno;
		iec = IEC__WRITE;
	}

	strbuf_release(&nbuf);

	return iec;
}

static enum checkout_helper__item_error_class write_item_to_disk(struct item *item)
{
	/* ASSUME ON WRITER[x] THREAD */
	/* ASSERT NO LOCK HELD */

	enum checkout_helper__item_error_class iec = IEC__OK;
	int did_fstat = 0;
	struct my_create_file_data d = { item, -1 };

	trace2_printf("%s:[item %d] write '%s'", t2_category_name,
		      item->helper_item_nr, item->path);

	if (raceproof_create_file(item->path, my_create_file_cb, &d)) {
		item->item_errno = errno;
		trace2_printf("XX: open failed [errno %d] '%s'",
			      item->item_errno, item->path);
		iec = IEC__OPEN;
		goto done;
	}

	assert(d.fd != -1);

	iec = smudge_and_write(d.fd, item);
	if (iec != IEC__OK) {
		close(d.fd);
		goto done;
	}

	if (fstat_is_reliable())
		did_fstat = (fstat(d.fd, &item->st) == 0);

	close(d.fd);

	if (!did_fstat && lstat(item->path, &item->st) < 0) {
		item->item_errno = errno;
		iec = IEC__LSTAT;
		goto done;
	}

done:
	FREE_AND_NULL(item->content);

	return iec;
}

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

static void *preload_thread_proc(void *_data)
{
	struct item *item;
	enum checkout_helper__item_error_class iec;

	trace2_thread_start("preload");

	pthread_mutex_lock(&main_mutex);
	while (1) {
		if (in_shutdown)
			break;

		if (preload_range.end >= item_vec.nr ||
		    preload_range.count >= preload_range_limit) {
			/*
			 * We've reached the current end of the queued
			 * items or we've filled our quota of
			 * in-memory blobs.  Wait for either condition
			 * to change before we try to preload another
			 * item.
			 */
			pthread_cond_wait(&preload_cond, &main_mutex);
			continue;
		}

		item = item_vec.array[preload_range.end];

		assert(item->item_state == ITEM_STATE__QUEUED);
		item->item_state = ITEM_STATE__LOADING;

		pthread_mutex_unlock(&main_mutex);
//		trace2_region_enter_printf(t2_category_name, "preload", NULL, "item[%d]", item->helper_item_nr);
		iec = preload_get_item(item);
//		trace2_region_leave_printf(t2_category_name, "preload", NULL, "item[%d]", item->helper_item_nr);
		pthread_mutex_lock(&main_mutex);

		item->item_error_class = iec;
		/*
		 * Regardless of error/success of reading the blob,
		 * we mark the item as loaded and include it in
		 * our range because we want to keep a contiguous
		 * series of rows "logically" in this state for quota
		 * purposes.
		 */
		item->item_state = ITEM_STATE__LOADED;
		preload_range.end++;
		preload_range.count++;

//		trace2_printf("%s:[item %d] signalling writers [%d,%d) [active %d]",
//			      t2_category_name, item->helper_item_nr,
//			      preload_range.end - preload_range.count,
//			      preload_range.end,
//			      nr_active_writers);

		if (nr_active_writers != writer_thread_pool_size)
			pthread_cond_signal(&writer_cond);
	}
	pthread_mutex_unlock(&main_mutex);

	trace2_thread_exit();
	return NULL;
}

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

static void *writer_thread_proc(void *_data)
{
	struct item *item;
	enum checkout_helper__item_error_class iec;
	int helper_item_nr;

	trace2_thread_start("writer");

	pthread_mutex_lock(&main_mutex);
	while (1) {
		if (in_shutdown)
			break;

		if (!preload_range.count) {
			/*
			 * There are no preloaded items currently
			 * ready in memory.
			 */
			pthread_cond_wait(&writer_cond, &main_mutex);
			continue;
		}

		/* Get the first preloaded item (ready for writing). */
		helper_item_nr = preload_range.end - preload_range.count;
		if (authorized_end != CHECKOUT_HELPER__AUTO_WRITE &&
		    helper_item_nr >= authorized_end) {
			/*
			 * If the client hasn't asked us to
			 * async-write this item yet, we must wait for
			 * them to do so.
			 */
			pthread_cond_wait(&writer_cond, &main_mutex);
			continue;
		}

		item = item_vec.array[helper_item_nr];
		assert(item->item_state == ITEM_STATE__LOADED);

		/*
		 * Remove it from the beginning of the preload range
		 * and let the preload thread know that there is quota
		 * space is available so that it can start preloading
		 * the next blob while we write this one.
		 */
		preload_range.count--;
		pthread_cond_signal(&preload_cond);

		/*
		 * Minor bookkeeping detail.  Only if the item was
		 * successfully loaded into memory do we try to write
		 * it to the worktree.  Otherwise, we just forward it
		 * to the done range.
		 */
		if (item->item_error_class == IEC__OK) {
			item->item_state = ITEM_STATE__WRITING;

			nr_active_writers++;
			pthread_mutex_unlock(&main_mutex);
//			trace2_region_enter_printf(t2_category_name, "writer", NULL, "item[%d]", item->helper_item_nr);
			iec = write_item_to_disk(item);
//			trace2_region_leave_printf(t2_category_name, "writer", NULL, "item[%d]", item->helper_item_nr);
			pthread_mutex_lock(&main_mutex);
			nr_active_writers--;

			// TODO decrement sum of loaded blobs if we go that way.

			item->item_error_class = iec;
		}

		item->item_state = ITEM_STATE__DONE;

		completed_count++;

		/*
		 * Signal the main thread than we have results for an item.
		 */
		pthread_cond_signal(&done_cond);
	}
	pthread_mutex_unlock(&main_mutex);

	trace2_thread_exit();
	return NULL;
}

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

typedef int (fn_helper_cmd)(const char *start_line);

struct helper_capability {
	const char *name;
	int client_has;
	fn_helper_cmd *pfn_helper_cmd;
};

/*
 * Async receive data for an array of items and add them to the
 * item_vec queue.
 *
 * We expect:
 *     command=<cmd>
 *     <binary data for item k>
 *     <binary data for item k+1>
 *     <binary data for item k+2>
 *     ...
 *     <flush>
 *
 * We do not send a response.
 */
static int helper_cmd__async_queue(const char *start_line)
{
	struct item *item = NULL;
	char *data_line;
	int len;
	struct checkout_helper__queue_item_record fixed_fields;
	char *variant;
	char *encoding;
	char *name;

//	trace2_printf("%s[%s]:", t2_category_name, start_line);

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (len < sizeof(struct checkout_helper__queue_item_record))
			BUG("%s[%s]: record too short (obs %d, exp %d)",
			    t2_category_name, start_line, len,
			    (int)sizeof(struct checkout_helper__queue_item_record));

		/*
		 * memcpy the fixed portion into a proper structure to
		 * guarantee memory alignment.
		 */
		memcpy(&fixed_fields, data_line,
		       sizeof(struct checkout_helper__queue_item_record));

		variant = data_line + sizeof(struct checkout_helper__queue_item_record);
		if (fixed_fields.len_encoding_name) {
			encoding = xmemdupz(variant, fixed_fields.len_encoding_name);
			variant += fixed_fields.len_encoding_name;
		} else
			encoding = NULL;

		name = xmemdupz(variant, fixed_fields.len_name);

		item = alloc_item(fixed_fields.pc_item_nr,
				  fixed_fields.helper_item_nr,
				  fixed_fields.ce_mode,
				  fixed_fields.attr_action,
				  fixed_fields.crlf_action,
				  fixed_fields.ident,
				  &fixed_fields.oid,
				  encoding,
				  name);
		item_vec_append(item);
	}

	return 0;
}

/*
 * Async receive an 'async write' request for a series of items.  This
 * allows the writer thread to write them to the worktree as soon as
 * they are available (or now if they are already preloaded).
 *
 * This is makes items[0, end) available for writing.
 *
 * We expect:
 *     command=<cmd>
 *     TODO ...params...
 *     <flush>
 *
 * We do not send a response.
 */
static int helper_cmd__async_write(const char *start_line)
{
	char *data_line;
	const char *token;
	int len;
	int end = -1;

	// TODO reformat the protocol to receive the message in one line/packet.

//	trace2_printf("%s[%s]:", t2_category_name, start_line);

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (skip_prefix(data_line, "end=", &token)) {
			end = strtol(token, NULL, 10);
			continue;
		}

		BUG("%s[%s]: unknown data line '%s'",
		    t2_category_name, start_line, data_line);
	}

	if (end == -1)
		BUG("%s[%s]: non-optional field 'end' omitted",
		    t2_category_name, start_line);

	set_async_write_on_items(end);

	return 0;
}

static void send_1_item(const struct item *item, int helper_item_nr)
{
	/* ASSERT NO LOCK REQUIRED (because item is DONE) */

	struct checkout_helper__item_result r;

	memset(&r, 0, sizeof(r));
	if (!item) {
		r.helper_item_nr = helper_item_nr;
		r.pc_item_nr = -1;
		r.item_error_class = IEC__INVALID_ITEM;
	} else {
		assert(item->helper_item_nr == helper_item_nr);

		r.helper_item_nr = item->helper_item_nr;
		r.pc_item_nr = item->pc_item_nr;
		r.item_error_class = item->item_error_class;
		r.item_errno = item->item_errno;
		r.st = item->st;
	}

	packet_write(1, (const char *)&r, sizeof(r));
}

/*
 * Synchronous request for the results on a single item.
 * Blocks until the item has been completely processed.
 * Returns various error states and/or lstat data for the
 * new file in the worktree.
 *
 * We expect:
 *     command=<cmd>
 *     TODO ...params...
 *     <flush>
 *
 * We respond:
 *     <result_k>
 *     <flush>
 *
 * Note that because we always write all items in queued order,
 * this effectively is a wait for all items upto and including
 * the requested one to be written.
 */
static int helper_cmd__sync_get1_item(const char *start_line)
{
	struct item *item = NULL;
	char *data_line;
	const char *token;
	int helper_item_nr = -1;
	int len;
	
//	trace2_printf("%s[%s]:", t2_category_name, start_line);

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (skip_prefix(data_line, "nr=", &token)) {
			helper_item_nr = strtol(token, NULL, 10);
			continue;
		}

		BUG("%s[%s]: unknown data line '%s'",
		    t2_category_name, start_line, data_line);
	}

	if (helper_item_nr == -1)
		BUG("%s[%s]: non-optional field 'nr' omitted",
		    t2_category_name, start_line);

	item = wait_for_done_item(helper_item_nr);
	send_1_item(item, helper_item_nr);

	return packet_flush_gently(1);
}

/*
 * Synchronous request for the results of a series of items.
 * Returns array [begin, end) of error states and/or lstat data.
 * Blocks as necessary, but sends results as they become available.
 *
 * We expect:
 *     command=<cmd>
 *     TODO ...params...
 *     <flush>
 *
 * We respond:
 *     <result_begin>
 *     <result_begin+1>
 *     ...
 *     <result_end-1>
 *     <flush>
 */
static int helper_cmd__sync_get_items(const char *start_line)
{
	struct item *item = NULL;
	char *data_line;
	const char *token;
	int len;
	int k;
	int begin = 0;
	int end = -1;
	
	// TODO reformat the protocol to receive the message in one line/packet.

//	trace2_printf("%s[%s]:", t2_category_name, start_line);

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (skip_prefix(data_line, "begin=", &token)) {
			begin = strtol(token, NULL, 10);
			continue;
		}

		if (skip_prefix(data_line, "end=", &token)) {
			end = strtol(token, NULL, 10);
			continue;
		}

		BUG("%s[%s]: unknown data line '%s'",
		    t2_category_name, start_line, data_line);
	}

	if (end == -1)
		BUG("%s[%s]: non-optional field 'end' omitted",
		    t2_category_name, start_line);

	for (k = begin; k < end; k++) {
		item = wait_for_done_item(k);
		send_1_item(item, k);
	}

	return packet_flush_gently(1);

}

static struct helper_capability caps[] = {
	{ "queue", 0, helper_cmd__async_queue },
	{ "write", 0, helper_cmd__async_write },
	{ "get1",  0, helper_cmd__sync_get1_item },
	{ "mget",  0, helper_cmd__sync_get_items },
	{ NULL, 0, NULL },
};

/*
 * Handle the subprocess protocol handshake as described in:
 * [] Documentation/technical/protocol-common.txt
 * [] Documentation/technical/long-running-process-protocol.txt
 *
 * Return 1 if we have a protocol error.
 */
static int do_protocol_handshake(void)
{
#define OUR_SUBPROCESS_VERSION "1"

	char *line;
	int len;
	int k;
	int b_support_our_version = 0;

	len = packet_read_line_gently(0, NULL, &line);
	if (len < 0 || !line || strcmp(line, "checkout--helper-client")) {
		error("server: subprocess welcome handshake failed: %s", line);
		return 1;
	}

	while (1) {
		const char *v;
		len = packet_read_line_gently(0, NULL, &line);
		if (len < 0 || !line)
			break;
		if (!skip_prefix(line, "version=", &v)) {
			error("server: subprocess version handshake failed: %s",
			      line);
			return 1;
		}
		b_support_our_version |= (!strcmp(v, OUR_SUBPROCESS_VERSION));
	}
	if (!b_support_our_version) {
		error("server: client does not support our version: %s",
		      OUR_SUBPROCESS_VERSION);
		return 1;
	}

	if (packet_write_fmt_gently(1, "checkout--helper-server\n") ||
	    packet_write_fmt_gently(1, "version=%s\n",
				    OUR_SUBPROCESS_VERSION) ||
	    packet_flush_gently(1)) {
		error("server: cannot write version handshake");
		return 1;
	}

	while (1) {
		const char *v;
		int k;

		len = packet_read_line_gently(0, NULL, &line);
		if (len < 0 || !line)
			break;
		if (!skip_prefix(line, "capability=", &v)) {
			error("server: subprocess capability handshake failed: %s",
			      line);
			return 1;
		}
		for (k = 0; caps[k].name; k++)
			if (!strcmp(v, caps[k].name))
				caps[k].client_has = 1;
	}

	for (k = 0; caps[k].name; k++)
		if (caps[k].client_has)
			if (packet_write_fmt_gently(1, "capability=%s\n",
						    caps[k].name)) {
				error("server: cannot write capabilities handshake: %s",
				      caps[k].name);
				return 1;
			}
	if (packet_flush_gently(1)) {
		error("server: cannot write capabilities handshake");
		return 1;
	}

	return 0;
}

static int server_loop(void)
{
	char *line;
	const char *cmd;
	int len;
	int k;

get_next_command:
	len = packet_read_line_gently(0, NULL, &line);
	if (len < 0 || !line)
		return 0;

	if (!skip_prefix(line, "command=", &cmd)) {
		error("%s: invalid sequence '%s'", t2_category_name, line);
		return 1;
	}

	for (k = 0; caps[k].name; k++) {
		if (!strcmp(cmd, caps[k].name)) {
			if (!caps[k].client_has) {
				/*
				 * The client sent a command that it didn't
				 * claim that it understood.
				 */
				error("%s: invalid command '%s'", t2_category_name, line);
				return 1;
			}

			if ((caps[k].pfn_helper_cmd)(line))
				return 1;

			goto get_next_command;
		}
	}

	/* The server doesn't know about this command. */
	error("%s: unsupported command '%s'", t2_category_name, line);
	return 1;
}

static const char * const checkout_helper_usage[] = {
	N_("git checkout-helper [<options>]"),
	NULL
};


int cmd_checkout__helper(int argc, const char **argv, const char *prefix)
{
	int err = 0;
	int w;
	int b_automatic = 0;

	struct option checkout_helper_options[] = {
		OPT_INTEGER(0, "child", &child_nr, N_("child number")),
		OPT_INTEGER('l', "preload", &preload_range_limit,
			    N_("preload limit")),
		OPT_INTEGER('w', "writers", &writer_thread_pool_size,
			    N_("number of concurrent writers")),
		OPT_BOOL('a', "automatic", &b_automatic,
			 N_("automatically write files")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(checkout_helper_usage,
				   checkout_helper_options);

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, checkout_helper_options,
			     checkout_helper_usage, 0);

	if (preload_range_limit < 1)
		preload_range_limit = DEFAULT_PRELOAD_RANGE_LIMIT;
	if (writer_thread_pool_size < 1)
		writer_thread_pool_size = DEFAULT_WRITER_THREAD_POOL_SIZE;
	if (b_automatic)
		set_async_write_on_items(CHECKOUT_HELPER__AUTO_WRITE);

	writer_thread_pool = xcalloc(writer_thread_pool_size, sizeof(pthread_t));

	snprintf(t2_category_name, sizeof(t2_category_name),
		 "helper[%02d]", child_nr);
	packet_trace_identity(t2_category_name);

	if (do_protocol_handshake())
		return 1;

	pthread_mutex_init(&main_mutex, NULL);
	pthread_cond_init(&preload_cond, NULL);
	pthread_cond_init(&writer_cond, NULL);
	pthread_cond_init(&done_cond, NULL);
	pthread_create(&preload_thread, NULL, preload_thread_proc, NULL);
	for (w = 0; w < writer_thread_pool_size; w++)
		pthread_create(&writer_thread_pool[w], NULL,
			       writer_thread_proc, NULL);

	err = server_loop();

	pthread_mutex_lock(&main_mutex);
	in_shutdown = 1;
	pthread_cond_signal(&preload_cond);
	pthread_cond_broadcast(&writer_cond);
	pthread_mutex_unlock(&main_mutex);

	pthread_join(preload_thread, NULL);
	for (w = 0; w < writer_thread_pool_size; w++)
	    pthread_join(writer_thread_pool[w], NULL);

	pthread_cond_destroy(&preload_cond);
	pthread_cond_destroy(&writer_cond);
	pthread_cond_destroy(&done_cond);
	pthread_mutex_destroy(&main_mutex);

	fflush(stderr);

	free_item_vec();

	return err;
}
