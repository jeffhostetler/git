#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "pkt-line.h"
#include "trace2.h"
#include "thread-utils.h"
#include "parse-options.h"
#include "object-store.h"
#include "parallel-checkout.h"

static char t2_category_name[100];
static int child_nr = -1;

struct item {
	struct item *next;

	/* These fields are available when the item is queued/defined. */
	int client_item_nr;
	struct object_id oid;
	struct conv_attrs ca;
	char *path;
	int mode;

	/* These fields are available after we try to prefetch the blob. */
	int fetch_error;
	unsigned long content_size;
	void *content;

	/* These fields are available after we try to write the file. */
	int open_errno;
	int write_errno;
};

static struct item *alloc_item(int item_nr, const struct object_id *oid,
			       int attr, int crlf, int ident, char *encoding,
			       char *path, int mode)
{
	struct item *item = xcalloc(1, sizeof(*item));

	item->client_item_nr = item_nr;

	oidcpy(&item->oid, oid);

	item->ca.attr_action = attr;
	item->ca.crlf_action = crlf;
	item->ca.ident = ident;
	item->ca.working_tree_encoding = encoding;

	item->path = path;
	item->mode = mode;

//	trace2_printf("%s: item [%d, %s, {%d, %d, %d, %s} {%s, %o}",
//		      t2_category_name,
//		      item->client_item_nr,
//		      oid_to_hex(&item->oid),
//		      item->ca.attr_action,
//		      item->ca.crlf_action,
//		      item->ca.ident,
//		      encoding ? encoding : "(nul)",
//		      path,
//		      mode);

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

struct item_list {
	struct item *head;
	struct item *tail;
	int count;
};

#define ITEM_LIST_INIT { NULL, NULL, 0 }

/*
 * The list of items that the client would like prefetched.  This is in the
 * order received *AND* in the order that the client will need the results.
 */
static struct item_list todo_list = ITEM_LIST_INIT;

/*
 * The list of items that we have prefetched.  The content of these blobs are
 * in-memory and ready for consumption.  Again, this list preserves the client
 * requested ordering.
 */
static struct item_list ready_list = ITEM_LIST_INIT;

/*
 * If we have spare cycles and plenty of memory, we should be able to keep
 * n items (blob contents) in memory at all times.  This value was chosen at
 * random.  It only needs to be large enough to keep the client from stalling
 * by waiting for us to prefetch a blob.
 */
#define READY_LIST_LIMIT 5

static pthread_mutex_t main_mutex;
static pthread_cond_t worker_cond;
static pthread_cond_t writer_cond;
static pthread_t worker_thread;
static int in_shutdown;

/*
 * Append item to a list. The list takes ownership of it.
 */
static void append_item_under_lock(struct item_list *list, struct item *item)
{
	/* assert holding main_mutex */

//	trace2_printf("append: %p %p %d", list->head, list->tail, list->count);

	item->next = NULL;

	if (!list->head) {
		assert(!list->tail);
		assert(!list->count);

		list->head = item;
		list->tail = item;
		list->count = 1;
	} else {
		assert(list->tail);
		assert(list->count);

		assert(item->client_item_nr > list->tail->client_item_nr);

		list->tail->next = item;
		list->tail = item;
		list->count++;
	}
}

/*
 * Append item to a list.
 *
 * Notify any waiting consumers.
 *
 * Optionally block the thread on the `wait_cond` while the list
 * is "full" (at the limit).
 */
static void append_item_with_limit(struct item_list *list, struct item *item,
				   int limit, pthread_cond_t *wait_cond,
				   pthread_cond_t *consumer_cond)
{
	pthread_mutex_lock(&main_mutex);
	append_item_under_lock(list, item);

	if (consumer_cond)
		pthread_cond_signal(consumer_cond);

	if (limit && wait_cond)
		while (list->count >= limit)
			pthread_cond_wait(wait_cond, &main_mutex);

	pthread_mutex_unlock(&main_mutex);
}

/*
 * Remove first item in the list and return it.
 * The caller owns it afterwards.
 */
static struct item *remove_head_under_lock(struct item_list *list)
{
	/* assert holding main_mutex */

	struct item *item;

//	trace2_printf("remove: %p %p %d", list->head, list->tail, list->count);

	assert(list->head);
	assert(list->tail);
	assert(list->count);

	item = list->head;

	list->head = list->head->next;
	list->count--;

	if (!list->head)
		list->tail = NULL;

	item->next = NULL;

	return item;
}

/*
 * Wait for an item to be avaliable in the list and return it.
 * Use the `wait_cond` to spin.
 *
 * Optionally signal `producer_cond` that space is available after we take one.
 *
 * Return NULL if we are shutting down.
 */
static struct item *wait_for_item_or_shutdown(struct item_list *list,
					      pthread_cond_t *wait_cond,
					      pthread_cond_t *producer_cond)
{
	pthread_mutex_lock(&main_mutex);
	while (1) {
		if (in_shutdown) {
			pthread_mutex_unlock(&main_mutex);
			return NULL;
		}

		if (list->count) {
			struct item *item = remove_head_under_lock(list);

			if (producer_cond)
				pthread_cond_signal(producer_cond);

			pthread_mutex_unlock(&main_mutex);
			return item;
		}

		pthread_cond_wait(wait_cond, &main_mutex);
	}
}

static void worker_fetch_item(struct item *item)
{
	struct object_info oi = OBJECT_INFO_INIT;
	enum object_type type;

//	trace2_printf("%s: fetching item[%d]", t2_category_name,
//		      item->client_item_nr);

	oi.typep = &type;
	oi.sizep = &item->content_size;
	oi.contentp = &item->content;

	if (oid_object_info_extended(the_repository, &item->oid, &oi, 0) < 0) {
		/*
		 * Just record that we had an error trying to fetch
		 * this item.  We cannot directly complain to the
		 * client at this point.  We can report an error
		 * later when the client asks for the content.
		 */
		item->fetch_error = 1;
		return;
	}

//	trace2_printf("%s: fetched item[%d] [size %d]", t2_category_name,
//		      item->client_item_nr, item->content_size);

	// TODO smudge the content.
}

static void *worker_thread_proc(void *_data)
{
	trace2_thread_start("worker");

	while (1) {
		struct item *item = wait_for_item_or_shutdown(&todo_list,
							      &worker_cond,
							      NULL);
		if (!item)
			break;

		worker_fetch_item(item);

		append_item_with_limit(&ready_list, item, READY_LIST_LIMIT,
				       &worker_cond, &writer_cond);
	}

	fflush(stderr);

	trace2_thread_exit();
	return NULL;
}

static int write_blob_to_disk(struct item *item)
{
	unsigned int mode = item->mode & 0100 ? 0777 : 0666;
	int fd = open(item->path, O_WRONLY | O_CREAT | O_EXCL, mode);

	if (fd == -1) {
		item->open_errno = errno;
		return -1;
	}

	if (write_in_full(fd, item->content, item->content_size) < 0) {
		item->write_errno = errno;
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

typedef int (fn_helper_cmd)(const char *start_line);

struct helper_capability {
	const char *name;
	int client_has;
	fn_helper_cmd *pfn_helper_cmd;
};

/*
 * Receive item data for a blob that the client wants preloaded into memory.
 * Create an `item` and append it to our `todo_list`.
 *
 * Signal the worker thread that there is work to do.
 *
 * We DO NOT send the client a response.
 */
static int helper_cmd__item(const char *start_line)
{
	struct item *item = NULL;
	char *data_line;
	const char *token;
	int len;

	int item_nr = -1;
	struct object_id oid;
	int have_oid = 0;
	int attr = 0;
	int crlf = 0;
	int ident = 0;
	char *encoding = NULL;
	char *path = NULL;
	int mode = 0;

//	trace2_printf("%s[%s]:", t2_category_name, start_line);

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (skip_prefix(data_line, "nr=", &token)) {
			item_nr = strtol(token, NULL, 10);
			continue;
		}

		if (skip_prefix(data_line, "oid=", &token)) {
			get_oid_hex(token, &oid);
			have_oid = 1;
			continue;
		}

		if (skip_prefix(data_line, "attr=", &token)) {
			attr = strtol(token, NULL, 10);
			continue;
		}
		if (skip_prefix(data_line, "crlf=", &token)) {
			crlf = strtol(token, NULL, 10);
			continue;
		}
		if (skip_prefix(data_line, "ident=", &token)) {
			ident = strtol(token, NULL, 10);
			continue;
		}
		if (skip_prefix(data_line, "encoding=", &token)) {
			encoding = xstrdup(token);
			continue;
		}

		if (skip_prefix(data_line, "path=", &token)) {
			path = xstrdup(token);
			continue;
		}

		if (skip_prefix(data_line, "mode=", &token)) {
			mode = strtol(token, NULL, 8);
			continue;
		}

		error("%s[%s]: unknown data line '%s'",
		      t2_category_name, start_line, data_line);
		return 1;
	}

	if (item_nr < 0) {
		error("%s[%s]: non-optional field 'nr' omitted",
		      t2_category_name, start_line);
		return 1;
	}
	if (!have_oid) {
		error("%s[%s]: non-optional field 'oid' omitted",
		      t2_category_name, start_line);
		return 1;
	}
	if (!path) {
		error("%s[%s]: non-optional field 'path' omitted",
		      t2_category_name, start_line);
		return 1;
	}

	item = alloc_item(item_nr, &oid,
			  attr, crlf, ident, encoding,
			  path, mode);

	append_item_with_limit(&todo_list, item, 0, NULL, &worker_cond);

	return 0;
}

/*
 * Receive synchronous `blob write` request from the client.  If the
 * blob is ready (prefetched and smudged), we can write it to the work
 * tree and return the error code to the client.  If the item is not
 * ready yet, wait for it.
 *
 * Return 1 if we send back an error.
 */
static int helper_cmd__write(const char *start_line)
{
	struct item *item = NULL;
	char *data_line;
	const char *token;
	int len;
	int item_nr = -1;
	int result_errno = 0;
	enum parallel_checkout_cmd_result pccr = PCCR_OK;

//	trace2_printf("%s[%s]:", t2_category_name, start_line);

	/*
	 * Read from client until we get a flush packet.
	 */
	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (skip_prefix(data_line, "nr=", &token)) {
			item_nr = strtol(token, NULL, 10);
			continue;
		}

		error("%s[%s]: unknown data line '%s'",
		      t2_category_name, start_line, data_line);
		pccr = PCCR_UNKNOWN_FIELD;
	}

	if (pccr != PCCR_OK)
		goto send_response;

	if (item_nr < 0) {
		error("%s[%s]: non-optional field 'nr' omitted",
		      t2_category_name, start_line);
		pccr = PCCR_MISSING_REQUIRED_FIELD;
		goto send_response;
	}

	/*
	 * Find the item with this 'item_nr' in the ready list.  This is not
	 * as bad as it sounds because everything is ordered and the client
	 * should ask us to write them the same order as it queued them to
	 * us, so it should be the first item in the list (if the list is
	 * not empty).
	 */
	item = wait_for_item_or_shutdown(&ready_list, &writer_cond,
					 &worker_cond);
	if (!item) {
		/*
		 * We only get a NULL if we are `in_shutdown` and because
		 * we run this in the foreground (main) thread, we should
		 * not be listening for commands from the client anyway.
		 */
		BUG("%s[%s]: invalid state", t2_category_name, start_line);
	}

	if (item_nr != item->client_item_nr) {
		error("%s[%s]: item_nr mismatch [rec %d exp %d] on '%s'",
		      t2_category_name, start_line,
		      item_nr, item->client_item_nr, item->path);
		free_item(item);
		pccr = PCCR_REQUEST_OUT_OF_ORDER;
		goto send_response;
	}

	if (item->fetch_error) {
		error("%s[%s]: fetch failed for '%s' on '%s'",
		      t2_category_name, start_line, oid_to_hex(&item->oid),
		      item->path);
		pccr = PCCR_BLOB_ERROR;
		goto send_response;
	}

	if (write_blob_to_disk(item) < 0) {
		if (item->open_errno) {
			error("%s[%s]: open failed on '%s': %s",
			      t2_category_name, start_line, item->path,
			      strerror(item->open_errno));
			result_errno = item->open_errno;
			pccr = PCCR_OPEN_ERROR;
			goto send_response;
		}

		if (item->write_errno) {
			error("%s[%s]: write failed on '%s': %s",
			      t2_category_name, start_line, item->path,
			      strerror(item->write_errno));
			result_errno = item->write_errno;
			pccr = PCCR_WRITE_ERROR;
			goto send_response;
		}

		BUG("%s[%s]: unknown error on '%s'",
		    t2_category_name, start_line, item->path);
	}

send_response:
	packet_write_fmt_gently(1, "write=%d\n", item->client_item_nr);
	packet_write_fmt_gently(1, "pccr=%d\n", (int)pccr);
	packet_write_fmt_gently(1, "errno=%d\n", result_errno);
	packet_flush_gently(1);

	free_item(item);

	return (pccr != PCCR_OK);
}

static struct helper_capability caps[] = {
	{ "item", 0, helper_cmd__item },
	{ "write", 0, helper_cmd__write },
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

top_of_loop:
	len = packet_read_line_gently(0, NULL, &line);
	if (len < 0 || !line)
		return 0;

	if (!skip_prefix(line, "command=", &cmd)) {
		error("invalid command start line '%s'", line);
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

			if ((caps[k].pfn_helper_cmd)(line))
				return 1;
			goto top_of_loop;
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
	int test = 0;

	struct option checkout_helper_options[] = {
		OPT_INTEGER(0, "child", &child_nr, N_("child number")),
		OPT_INTEGER('t', "test", &test, N_("test mode")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(checkout_helper_usage,
				   checkout_helper_options);

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, checkout_helper_options,
			     checkout_helper_usage, 0);
	snprintf(t2_category_name, sizeof(t2_category_name),
		 "helper[%02d]", child_nr);
	packet_trace_identity(t2_category_name);

	if (!test)
	if (do_protocol_handshake())
		return 1;

	pthread_mutex_init(&main_mutex, NULL);
	pthread_cond_init(&worker_cond, NULL);
	pthread_cond_init(&writer_cond, NULL);
	pthread_create(&worker_thread, NULL, worker_thread_proc, NULL);

	if (test) {
		int k;
		const char *hex_oid[] = { "141869db80077b8c5a2992c877f40058515f66f2",
					  "ffc61ac9f84f65a4166e9fb7a631385da1280199",
					  "c380a78a57e1d5551dde2725b7384772e730249a",
					  "dade0a232e4c0dd9383cedbbe4f3600c67c1a2bb",
					  "8a82b1107cb158754fac37812475719210ae709d",
		};
		for (k = 0; k < 5; k++) {
			struct object_id oid;
			struct item *item;

			get_oid_hex(hex_oid[k], &oid);
			item = alloc_item(k, &oid, 0, 0, 0, NULL, "test_path", 0666);

			append_item_with_limit(&todo_list, item, 0, NULL, &worker_cond);
		}

		for (k = 0; k < 5; k++) {
			struct item *item;
			int result;

			item = wait_for_item_or_shutdown(&ready_list,
							 &writer_cond,
							 &worker_cond);

			trace2_printf("%s: test [k %d] got [%d, %s]",
				      t2_category_name, k, item->client_item_nr,
				      oid_to_hex(&item->oid));
			result = write_blob_to_disk(item);
			trace2_printf("%s: [k %d] test write [result %d]",
				      t2_category_name, k, result);

			err |= (result < 0);
		}
	}
	else {
		err = server_loop();
	}

	pthread_mutex_lock(&main_mutex);
	in_shutdown = 1;
	pthread_cond_signal(&worker_cond);
	pthread_cond_signal(&writer_cond);
	pthread_mutex_unlock(&main_mutex);

	pthread_join(worker_thread, NULL);

	pthread_cond_destroy(&worker_cond);
	pthread_cond_destroy(&writer_cond);
	pthread_mutex_destroy(&main_mutex);

	fflush(stderr);

	return err;
}
