#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "pkt-line.h"
#include "trace2.h"
#include "thread-utils.h"
#include "parse-options.h"
#include "object-store.h"
#include "checkout--helper.h"
#include "json-writer.h"

/*
 * This is used as a label prefix in all error messages
 * and in any trace2 messages to identify this child
 * process.
 */
static char t2_category_name[100];
static int child_nr = -1;

/*
 * New --> Queued --> Loading --> Loaded --> Writing --> Done
 */
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
	int skip;
	int checked_smudge;
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
 * The total number of items that we had errors on.
 * The total number of items that we actually smudged.
 *
 * These are for tracing only.
 */
static int total_error_count;
static int total_smudged_count;

/*
 * We always start writing the items in the order queued and expect
 * the client to request them in that order.  Even when we have multiple
 * writers, the items are pulled from the queue in that order.
 *
 * Therefore, we only keep track of the number of items authorized for
 * writing and assume that everything in [0, end) is authorized.
 *
 * When in async mode, we set it to maxint. 
 */
#define ASYNC_MODE_VALUE maximum_signed_value_of_type(int)

static int authorized_end;

#define IS_ASYNCHRONOUS() (authorized_end == ASYNC_MODE_VALUE)
#define IS_SYNCHRONOUS() (authorized_end != ASYNC_MODE_VALUE)

/*
 * The first item that should be returned in the next rogress request
 * in asynch mode.
 */
static int progress_begin;

static pthread_mutex_t main_mutex;
static pthread_t preload_thread;
static pthread_t *writer_thread_pool;

/*
 * Dynamic count of the number of busy writers.
 *
 * This is mainly for debugging, but does allow us to avoid sending
 * a pthread signal when we already know all the writers are busy.
 */
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
static int preload_range_limit = DEFAULT_PARALLEL_CHECKOUT_PRELOAD;

/*
 * Number of writer threads in the thread pool.
 *
 * The size of the thread pool is a function of what type of checkout
 * being attempted.  For example, a clone into an empty worktree should
 * be able to write n files in parallel without worrying about stepping
 * on uncommitted changes; whereas a branch switch might need to be more
 * careful and populate files sequentially.
 */
static int writer_thread_pool_size = DEFAULT_PARALLEL_CHECKOUT_WRITERS;

/*
 * Allow verbose trace2 logging when this environment variable is set.
 * This is primarily for the test suite to let us confirm that checkout--helper
 * actually did the work.
 *
 * These brackets are somewhat arbitrary.
 */
#define TEST_VERBOSE_LEVEL__OFF           0
#define TEST_VERBOSE_LEVEL__ERRORS        1
#define TEST_VERBOSE_LEVEL__VERBOSE       2
#define TEST_VERBOSE_LEVEL__VERY_VERBOSE  3

static int test_verbose = TEST_VERBOSE_LEVEL__OFF;

static void set_test_verbose(void)
{
	const char *value = getenv("GIT_TEST_CHECKOUT_HELPER_VERBOSE");

	if (!trace2_is_enabled())
		return;

	if (value) {
		int ivalue = strtol(value, NULL, 10);
		if (ivalue > 0)
			test_verbose = ivalue;
	}
}

#define KV(kv)     #kv, kv
#define KPV(p, kv) #kv, p->kv

static void verbose_super_prefixed_path(struct json_writer *jw, const char *path)
{
	const char *super_prefix = get_super_prefix();

	if (super_prefix) {
		struct strbuf buf = STRBUF_INIT;

		strbuf_addstr(&buf, super_prefix);
		strbuf_addstr(&buf, path);

		jw_object_string(jw, "super_path", buf.buf);

		strbuf_release(&buf);
	}
}

static void verbose_log_queued(const struct item *item)
{
	struct json_writer jw = JSON_WRITER_INIT;
	const char *oid = oid_to_hex(&item->oid);
	const struct conv_attrs *ca = &item->ca;

	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, KPV(item, helper_item_nr));
	jw_object_intmax(&jw, KPV(item, pc_item_nr));
	jw_object_intmax(&jw, KPV(item, mode));
	jw_object_string(&jw, KV(oid));
	jw_object_string(&jw, KPV(item, path));
	jw_object_inline_begin_object(&jw, "ca");
	jw_object_intmax(&jw, KPV(ca, attr_action));
	jw_object_intmax(&jw, KPV(ca, crlf_action));
	if (ca->working_tree_encoding)
		jw_object_string(&jw, KPV(ca, working_tree_encoding));
	jw_end(&jw);
	jw_end(&jw);

	trace2_data_json(t2_category_name, NULL, "queued", &jw);
	jw_release(&jw);
}

static void verbose_log_preload_failed(const struct item *item)
{
	struct json_writer jw = JSON_WRITER_INIT;
	const char *oid = oid_to_hex(&item->oid);

	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, KPV(item, helper_item_nr));
	jw_object_string(&jw, KV(oid));
	jw_object_string(&jw, KPV(item, path));
	jw_end(&jw);

	trace2_data_json(t2_category_name, NULL, "preload_failed", &jw);
	jw_release(&jw);
}

static void verbose_log_preloaded(const struct item *item)
{
	struct json_writer jw = JSON_WRITER_INIT;
	const char *oid = oid_to_hex(&item->oid);
	intmax_t size = item->content_size;

	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, KPV(item, helper_item_nr));
	jw_object_string(&jw, KV(oid));
	jw_object_intmax(&jw, KV(size));
	jw_object_string(&jw, KPV(item, path));
	jw_end(&jw);

	trace2_data_json(t2_category_name, NULL, "preloaded", &jw);
	jw_release(&jw);
}

static void verbose_log_smudged(intmax_t helper_item_nr,
				intmax_t old_size,
				intmax_t new_size,
				const char *path)
{
	struct json_writer jw = JSON_WRITER_INIT;

	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, KV(helper_item_nr));
	jw_object_intmax(&jw, KV(old_size));
	jw_object_intmax(&jw, KV(new_size));
	jw_object_string(&jw, KV(path));
	verbose_super_prefixed_path(&jw, path);
	jw_end(&jw);

	trace2_data_json(t2_category_name, NULL, "smudged", &jw);
	jw_release(&jw);
}

static void verbose_log_writing(intmax_t helper_item_nr, const char *path)
{
	struct json_writer jw = JSON_WRITER_INIT;

	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, KV(helper_item_nr));
	jw_object_string(&jw, KV(path));
	verbose_super_prefixed_path(&jw, path);
	jw_end(&jw);

	trace2_data_json(t2_category_name, NULL, "writing", &jw);
	jw_release(&jw);
}

static void verbose_log_open_failed(intmax_t helper_item_nr,
				    intmax_t item_errno,
				    const char *path)
{
	struct json_writer jw = JSON_WRITER_INIT;

	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, KV(helper_item_nr));
	jw_object_intmax(&jw, KV(item_errno));
	jw_object_string(&jw, KV(path));
	verbose_super_prefixed_path(&jw, path);
	jw_end(&jw);

	trace2_data_json(t2_category_name, NULL, "open_failed", &jw);
	jw_release(&jw);
}

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

	return item;
}

static void free_item(struct item *item)
{
	if (!item)
		return;

	/*
	 * The `ca.working_tree_encoding` field is defined as `const` and it is
	 * is always assigned a value from the statically initialized table in
	 * convert.c.  However, here in `checkout--helper` we have to cast-away
	 * the const and actually free it, because we allocated it from a field
	 * we received over the wire.
	 */
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
	 * We have a new item in the item queue, but don't bother
	 * sending a pthread signal if we already know that the
	 * preload read is busy and not blocked.  (We know that
	 * because it hasn't filled its preload quota yet.)
	 */
	if (preload_range.count < preload_range_limit)
	    pthread_cond_signal(&preload_cond);

	pthread_mutex_unlock(&main_mutex);
}

/*
 * Return the first item not marked DONE.
 *
 * Our caller can report results to the client process for
 * the DONE items in [begin, <rval>).
 *
 * We take the lock while quickly scanning the item array.
 * This gives us a snapshot of the set of DONE items.  Our
 * caller should report that set and not try to get clever
 * by repeatedly locking/unlocking/spinning.
 */
static int progress_first_not_done(void)
{
	/* ASSUME ON MAIN THREAD */

	int k;

	assert(IS_ASYNCHRONOUS());

	pthread_mutex_lock(&main_mutex);

	for (k = progress_begin; k < item_vec.nr; k++) {
		if (item_vec.array[k]->item_state != ITEM_STATE__DONE)
			break;
	}

	pthread_mutex_unlock(&main_mutex);

	return k;
}

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

	oi.typep = &type;
	oi.sizep = &item->content_size;
	oi.contentp = &item->content;

	if (oid_object_info_extended(the_repository, &item->oid, &oi, 0) < 0) {
		if (test_verbose >= TEST_VERBOSE_LEVEL__ERRORS)
			verbose_log_preload_failed(item);

		return IEC__LOAD;
	}

	if (test_verbose >= TEST_VERBOSE_LEVEL__VERY_VERBOSE)
		verbose_log_preloaded(item);

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

static int do_smudge_item(struct item *item)
{
	unsigned long original_size = item->content_size;
	size_t smudged_size;
	struct strbuf nbuf = STRBUF_INIT;
	enum conv_attrs_classification c = classify_conv_attrs(&item->ca);
	int did_smudge = 0;

	if (item->checked_smudge)
		return 0;

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

	/*
	 * See CA_CLASS_STREAMABLE.
	 *
	 * We always use `convert_to_working_tree_ca` to do in-core
	 * smudging and do not attempt to use `get_stream_filter_ca`
	 * to stream filter it.
	 *
	 * In synch mode, the whole point is to preload the blob into
	 * memory and have the contents available for writing to the
	 * worktree as soon as the foreground process is ready for it.
	 * So we let the preload thread also pre-smudge it.  This lets
	 * us only block the foreground process for the duration of
	 * the actual open/write/close.  This implies that we can't
	 * stream it.
	 *
	 * In asynch mode, the whole point is to have n writers trying
	 * to populate the worktree as fast as possible.  Smudging is
	 * thread-safe, so we let the writer threads do the work
	 * rather the preload thread.  Therefore, it should be
	 * possible to stream filter or in-core filter them.
	 *
	 * TODO investigate optional stream filtering when in
	 * asynch mode.
	 */

	// TODO The parallel-checkout code was being written while
	// TODO the checkout-metadata was being added.  During my
	// TODO rebase/merge I ignored the meta data args.  IIUC
	// TODO they are only used to give filter processes context.
	// TODO Since parallel-checkout considers such filtered files
	// TODO as not parallel-eligible, I do not think we need for
	// TODO parallel-checkout.c to pass the meta data to the helper.
	// TODO And we should just pass NULL here.
	// TODO
	// TODO Confirm my understanding of this.

	if (convert_to_working_tree_ca(&item->ca, item->path, /* See [1] */
				       item->content, original_size,
				       &nbuf, NULL)) {
		did_smudge = 1;

		free(item->content);
		item->content = strbuf_detach(&nbuf, &smudged_size);
		item->content_size = (unsigned long)smudged_size;

		if (test_verbose >= TEST_VERBOSE_LEVEL__VERBOSE)
			verbose_log_smudged(item->helper_item_nr,
					    original_size, smudged_size,
					    item->path);
	}

	item->checked_smudge = 1;

	strbuf_release(&nbuf);

	return did_smudge;
}

static enum checkout_helper__item_error_class write_item_to_disk(struct item *item)
{
	/* ASSUME ON WRITER[x] THREAD */
	/* ASSERT NO LOCK HELD */

	enum checkout_helper__item_error_class iec = IEC__OK;
	int did_fstat = 0;
	struct my_create_file_data d = { item, -1 };

	/*
	 * We don't care if the item was actually smudged, but only that
	 * either the preload or the writer thread did try to smudge it.
	 */
	assert(item->checked_smudge);

	if (test_verbose >= TEST_VERBOSE_LEVEL__VERY_VERBOSE)
		verbose_log_writing(item->helper_item_nr, item->path);

	if (raceproof_create_file(item->path, my_create_file_cb, &d)) {
		item->item_errno = errno;

		if (test_verbose >= TEST_VERBOSE_LEVEL__ERRORS)
			verbose_log_open_failed(item->helper_item_nr,
						item->item_errno,
						item->path);

		iec = IEC__OPEN;
		goto done;
	}

	assert(d.fd != -1);

	if (write_in_full(d.fd, item->content, item->content_size) < 0) {
		item->item_errno = errno;
		iec = IEC__WRITE;

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

static void *preload_thread_proc(void *_data)
{
	struct item *item;
	enum checkout_helper__item_error_class iec;
	int did_smudge;

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
		if (!item->skip) {
			item->item_state = ITEM_STATE__LOADING;

			pthread_mutex_unlock(&main_mutex);

			iec = preload_get_item(item);

			/* When SYNC go ahead and smudge it now. */
			did_smudge = (iec == IEC__OK &&
				      IS_SYNCHRONOUS() &&
				      do_smudge_item(item));

			pthread_mutex_lock(&main_mutex);

			total_smudged_count += did_smudge;
			item->item_error_class = iec;
		}

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

		if (nr_active_writers != writer_thread_pool_size)
			pthread_cond_signal(&writer_cond);
	}
	pthread_mutex_unlock(&main_mutex);

	trace2_thread_exit();
	return NULL;
}

static void *writer_thread_proc(void *_data)
{
	struct item *item;
	enum checkout_helper__item_error_class iec;
	int helper_item_nr;
	int did_smudge;

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
		if (IS_SYNCHRONOUS() &&
		    helper_item_nr >= authorized_end) {
			/*
			 * In sync-mode, we must wait for the foreground
			 * process to request this item be written.
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
		 * Only if the item was successfully loaded into
		 * memory do we try to write it to the worktree.
		 * Otherwise, we just forward it to the done range.
		 */
		if (!item->skip && item->item_error_class == IEC__OK) {
			item->item_state = ITEM_STATE__WRITING;

			nr_active_writers++;

			pthread_mutex_unlock(&main_mutex);

			did_smudge = do_smudge_item(item);
			iec = write_item_to_disk(item);

			pthread_mutex_lock(&main_mutex);

			nr_active_writers--;
			total_smudged_count += did_smudge;
			item->item_error_class = iec;
		}

		if (item->item_error_class != IEC__OK)
			total_error_count++;

		/*
		 * Mark the item DONE.
		 *
		 * From this point forward we treat this item as read-only.
		 */
		item->item_state = ITEM_STATE__DONE;

		/*
		 * Signal the main thread than we have results for an item.
		 */
		pthread_cond_signal(&done_cond);
	}
	pthread_mutex_unlock(&main_mutex);

	trace2_thread_exit();
	return NULL;
}

typedef int (fn_helper_cmd)(void);

struct helper_capability {
	const char *name;
	int client_has;
	fn_helper_cmd *pfn_helper_cmd;
};

/*
 * Receive data for an array of items and add them to the item_vec
 * queue.
 *
 * We expect:
 *     command=<cmd>
 *     <binary 'checkout_helper__queue_item_record + variant-data' for item k>
 *     <binary 'checkout_helper__queue_item_record + variant-data' for item k+1>
 *     <binary 'checkout_helper__queue_item_record + variant-data' for item k+2>
 *     ...
 *     <flush>
 *
 * We do not send a response.
 *
 * The client process is free to send one big batch or to send
 * multiple batches of items.
 */
static int helper_cmd__queue(void)
{
	struct item *item = NULL;
	char *data_line;
	int len;
	struct checkout_helper__queue_item_record fixed_fields;
	char *variant;
	char *encoding;
	char *name;

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (len < sizeof(struct checkout_helper__queue_item_record))
			BUG("%s[queue]: record too short (obs %d, exp %d)",
			    t2_category_name, len, (int)sizeof(fixed_fields));

		/*
		 * memcpy the fixed portion into a proper structure to
		 * guarantee memory alignment.
		 */
		memcpy(&fixed_fields, data_line,
		       sizeof(struct checkout_helper__queue_item_record));

		variant = data_line + sizeof(fixed_fields);
		if (fixed_fields.len_encoding_name) {
			encoding = xmemdupz(variant,
					    fixed_fields.len_encoding_name);
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

		if (test_verbose >= TEST_VERBOSE_LEVEL__VERY_VERBOSE)
			verbose_log_queued(item);
	}

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
 * This command verb is used by the client (foreground) process to
 * request incremental progress reports and results from this helper
 * process.
 *
 * We respond with the results for zero or more completed items (since
 * the previous progress report).  A response with zero items just
 * means that there are no new results since the last request.  The
 * client knows how many items were assigned to this helper process
 * and should keep polling until it receives results for all of them.
 *
 * We expect:
 *     command=<cmd>
 *     <flush>
 *
 * We respond:
 *     [<result_k>
 *      [<result_k+1>
 *       [...]]]
 *     <flush>
 *
 * This version always sends the full set of newly completed items.
 * [] It does not wait or have a timeout to accumulate a minimum number
 *    of items.
 * [] It does not have a maximum number of items (chunk size) parameter.
 * The assumption is that the client process is polling over 1 or more
 * helper processes (and displaying console progress) and needs to
 * control such things.  So for simplicity, we have omitted such things
 * for now.
 */
static int helper_cmd__async_progress(void)
{
	char *data_line;
	int len;
	int end;
	int k;

	assert(IS_ASYNCHRONOUS());

	/*
	 * Eat the flush packet.  Since we currently don't expect any
	 * parameters, eat them too.
	 */
	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;
	}

	end = progress_first_not_done();

	/*
	 * All items in contiguous range [begin, end) are marked DONE
	 * and are treated as read-only.  Therefore, we can access them
	 * without holding the lock.
	 */
	for (k = progress_begin; k < end; k++)
		send_1_item(item_vec.array[k], k);

	/*
	 * The next progress response will start where we left off.
	 */
	progress_begin = end;

	return packet_flush_gently(1);
}

static struct item *do_sync_write(int helper_item_nr)
{
	/* ASSUME ON MAIN THREAD */

	struct item *item;
	int k;

	pthread_mutex_lock(&main_mutex);

	if (helper_item_nr >= item_vec.nr) {
		/* Cause an IEC__INVALID_ITEM to be returned to client. */
		pthread_mutex_unlock(&main_mutex);
		return NULL;
	}

	assert(authorized_end <= helper_item_nr);

	/*
	 * In sync mode "authorized_end" means that the files in the range
	 * [0, end) have already been written and individual results sent
	 * back to the client.
	 *
	 * Anything in the range [end, helper_item_nr) should be skipped.
	 *
	 * And we should wait for helper_item_nr to be written (by
	 * extending the range just past this item).
	 *
	 * To minimize thread problems, we just set the "skip" bit on
	 * the items that we don't want and leave the state unchanged
	 * and keep it queued as it is.  Our background threads will
	 * see this bit before they start processing the item and
	 * short-cut the work and advance the item to the next state.
	 */
	for (k = authorized_end; k < helper_item_nr; k++) {
		item_vec.array[k]->skip = 1;
		authorized_end++;
		pthread_cond_signal(&writer_cond);
	}

	authorized_end = helper_item_nr + 1;
	pthread_cond_signal(&writer_cond);

	item = item_vec.array[helper_item_nr];
	while (item->item_state != ITEM_STATE__DONE)
		pthread_cond_wait(&done_cond, &main_mutex);

	pthread_mutex_unlock(&main_mutex);

	return item;
}

/*
 * This command verb is used by the client (foreground) process to
 * request that we try to write a single item to the worktree and
 * return the result.  This is only used when in sync mode and not
 * in async mode.
 *
 * We require that the client request items in the order that they
 * were queued (because that is how we have preloaded the blobs (and
 * is the whole point of this exercise)).
 *
 * If the client skips an item, we assume that the skipped items
 * should be discarded.  In entry.c:checkout_entry() there are several
 * early returns that avoid calling entry.c:write_entry() and we
 * assume that is the reason for the skip.
 *
 * We expect:
 *     command=<cmd>
 *     <binary 'checkout_helper__synch__write_record' for item k>
 *     <flush>
 *
 * We respond with:
 *     <result_k>
 *     <flush>
 */
static int helper_cmd__sync_write(void)
{
	char *data_line;
	int len;
	struct checkout_helper__sync__write_record rec;
	struct item *item;

	assert(IS_SYNCHRONOUS());

	len = packet_read_line_gently(0, NULL, &data_line);
	if (len != sizeof(rec) || !data_line)
		BUG("%s[sync_write]: invalid data-line", t2_category_name);
	memcpy(&rec, data_line, sizeof(rec));

	/* Eat the flush packet (and any other unexpected data lines). */
	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;
	}

	item = do_sync_write(rec.helper_item_nr);
	send_1_item(item, rec.helper_item_nr);

	return packet_flush_gently(1);
}

static struct helper_capability caps[] = {
	{ "queue",          0, helper_cmd__queue },
	{ "async_progress", 0, helper_cmd__async_progress },
	{ "sync_write",     0, helper_cmd__sync_write },
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
				error("%s: invalid command '%s'",
				      t2_category_name, line);
				return 1;
			}

			if ((caps[k].pfn_helper_cmd)())
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
	int b_asynchronous = 0;

	struct option checkout_helper_options[] = {
		OPT_INTEGER(0, "child", &child_nr, N_("child number")),
		OPT_INTEGER('l', "preload", &preload_range_limit,
			    N_("preload limit")),
		OPT_INTEGER('w', "writers", &writer_thread_pool_size,
			    N_("number of concurrent writers")),
		OPT_BOOL('a', "asynch", &b_asynchronous,
			 N_("asynchronously write files")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(checkout_helper_usage,
				   checkout_helper_options);

	preload_range_limit = core_parallel_checkout_preload;
	writer_thread_pool_size = core_parallel_checkout_writers;

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, checkout_helper_options,
			     checkout_helper_usage, 0);

	if (preload_range_limit < 1)
		preload_range_limit = DEFAULT_PARALLEL_CHECKOUT_PRELOAD;
	if (writer_thread_pool_size < 1)
		writer_thread_pool_size = DEFAULT_PARALLEL_CHECKOUT_WRITERS;

	if (b_asynchronous) {
		authorized_end = ASYNC_MODE_VALUE;
		trace2_cmd_mode("asynch");
	} else {
		trace2_cmd_mode("synch");
	}

	/*
	 * Override the packed-git memory limits.
	 *
	 * The defaults for a 64 bit platform are too big on gigantic
	 * repositories because we might 500GB of packfiles linger in
	 * memory.  And if we also have _n_ `checkout_helper` processes
	 * running in parallel, it can overload system memory.
	 *
	 * TODO Revisit how we override these 2 parameters.  It helps
	 * keep _n_ helper processes from using 99+% of system memory
	 * and making the machine almost non-responsive, but we should
	 * tune this better.  These values were chosen because they are
	 * the 32 bit defaults.  Perhaps pass them as command line args
	 * so that parallel-checkout.c can set them based upon the total
	 * number of helpers that it starts.
	 */
	if (packed_git_window_size > (1024L * 1024L * 32L))
		packed_git_window_size = 1024L * 1024L * 32L;
	if (packed_git_limit > (1024L * 1024L * 1024L))
		packed_git_limit = 1024L * 1024L * 1024L;

	trace2_data_intmax(t2_category_name, NULL, "packed/window",
			   packed_git_window_size);
	trace2_data_intmax(t2_category_name, NULL, "packed/limit",
			   packed_git_limit);

	set_test_verbose();

	writer_thread_pool = xcalloc(writer_thread_pool_size,
				     sizeof(pthread_t));

	snprintf(t2_category_name, sizeof(t2_category_name),
		 "helper[%02d]", child_nr);
	packet_trace_identity(t2_category_name);

	trace2_data_intmax(t2_category_name, NULL, "param/preload",
			   preload_range_limit);
	trace2_data_intmax(t2_category_name, NULL, "param/writers",
			   writer_thread_pool_size);

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

	trace2_data_intmax(t2_category_name, NULL, "item/count", item_vec.nr);
	if (total_smudged_count)
		trace2_data_intmax(t2_category_name, NULL, "item/smudge_count",
				   total_smudged_count);
	if (total_error_count)
		trace2_data_intmax(t2_category_name, NULL, "item/error_count",
				   total_error_count);

	free_item_vec();

	return err;
}
