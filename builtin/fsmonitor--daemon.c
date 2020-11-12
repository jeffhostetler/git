/*
 * built-in fsmonitor daemon
 *
 * Monitor filesystem changes to update the Git index intelligently.
 *
 * Copyright (c) 2019 Johannes Schindelin
 */

#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "fsmonitor.h"
#include "fsmonitor--daemon.h"
#include "simple-ipc.h"
#include "khash.h"

static const char * const builtin_fsmonitor__daemon_usage[] = {
	N_("git fsmonitor--daemon [--query] <token>"),
	N_("git fsmonitor--daemon <command-mode> [<options>...]"),
	NULL
};

#ifndef HAVE_FSMONITOR_DAEMON_BACKEND
#define FSMONITOR_DAEMON_IS_SUPPORTED 0

static int fsmonitor_query_daemon(const char *unused_since,
				  struct strbuf *unused_answer)
{
	die(_("no native fsmonitor daemon available"));
}

static int fsmonitor_run_daemon(void)
{
	die(_("no native fsmonitor daemon available"));
}

static int fsmonitor_daemon_is_running(void)
{
	warning(_("no native fsmonitor daemon available"));
	return 0;
}

static int fsmonitor_stop_daemon(void)
{
	warning(_("no native fsmonitor daemon available"));
	return 0;
}

static int fsmonitor_flush_daemon(void)
{
	warning(_("no native fsmonitor daemon available"));
	return 0;
}
#else
#define FSMONITOR_DAEMON_IS_SUPPORTED 1

/*
 * Global state loaded from config.
 */
#define FSMONITOR__IPC_THREADS "fsmonitor.ipcthreads"
static int fsmonitor__ipc_threads = 8;

static int fsmonitor_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, FSMONITOR__IPC_THREADS)) {
		int i = git_config_int(var, value);
		if (i < 1)
			return error(_("value of '%s' out of range: %d"),
				     FSMONITOR__IPC_THREADS, i);
		fsmonitor__ipc_threads = i;
		return 0;
	}

	return git_default_config(var, value, cb);
}

static enum fsmonitor_cookie_item_result fsmonitor_wait_for_cookie(
	struct fsmonitor_daemon_state *state)
{
	int fd;
	struct fsmonitor_cookie_item cookie;
	char *cookie_path;
	struct strbuf cookie_filename = STRBUF_INIT;
	int my_cookie_seq;

	pthread_mutex_lock(&state->main_lock);

	my_cookie_seq = state->cookie_seq++;

	// TODO This assumes that .git is a directory.  We need to
	// TODO figure out where to put the cookie files when .git
	// TODO is a link (such as for submodules).  Especially since
	// TODO the link directory may not be under the cone of the
	// TODO worktree and thus not registered for notifications.

	strbuf_addstr(&cookie_filename, FSMONITOR_COOKIE_PREFIX);
	strbuf_addf(&cookie_filename, "%i-%i", getpid(), my_cookie_seq);
	cookie.name = strbuf_detach(&cookie_filename, NULL);
	cookie.result = FCIR_INIT;
	hashmap_entry_init(&cookie.entry, strhash(cookie.name));

	// TODO Putting the address of a stack variable into a global
	// TODO hashmap feels dodgy.  Granted, the `handle_client()`
	// TODO stack frame is in a thread that will block on this
	// TODO returning, but do all coce paths guarantee that it is
	// TODO removed from the hashmap before this stack frame returns?

	hashmap_add(&state->cookies, &cookie.entry);
	pthread_mutex_unlock(&state->main_lock);

	cookie_path = git_pathdup("%s", cookie.name);
	fd = open(cookie_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) {
		close(fd);
		unlink_or_warn(cookie_path);

		// TODO We need a way to timeout this loop in case the
		// TODO listener thread never sees the event for the
		// TODO cookie file.
		// TODO
		// TODO Since we don't have pthread_cond_timedwait() on
		// TODO Windows, maybe let the listener thread (which is
		// TODO getting regular events) set a FCIR_TIMEOUT bit...

		pthread_mutex_lock(&state->main_lock);
		while (cookie.result == FCIR_INIT)
			pthread_cond_wait(&state->cookies_cond,
					  &state->main_lock);

		hashmap_remove(&state->cookies, &cookie.entry, NULL);
		pthread_mutex_unlock(&state->main_lock);
	} else {
		error_errno(_("could not create fsmonitor cookie '%s'"),
			    cookie_path);

		pthread_mutex_lock(&state->main_lock);
		cookie.result = FCIR_ERROR;
		hashmap_remove(&state->cookies, &cookie.entry, NULL);
		pthread_mutex_unlock(&state->main_lock);
	}

	free((char*)cookie.name);
	free(cookie_path);
	return cookie.result;
}

void fsmonitor_cookie_mark_seen(struct fsmonitor_daemon_state *state,
				const struct string_list *cookie_names)
{
	int k;
	int nr_seen = 0;

	pthread_mutex_lock(&state->main_lock);

	for (k = 0; k < cookie_names->nr; k++) {
		struct fsmonitor_cookie_item key;
		struct fsmonitor_cookie_item *cookie;

		key.name = cookie_names->items[k].string;
		hashmap_entry_init(&key.entry, strhash(key.name));

		cookie = hashmap_get_entry(&state->cookies, &key, entry, NULL);
		if (cookie) {
			cookie->result = FCIR_SEEN;
			nr_seen++;
		}
	}

	if (nr_seen)
		pthread_cond_broadcast(&state->cookies_cond);

	pthread_mutex_unlock(&state->main_lock);
}

void fsmonitor_cookie_abort_all(struct fsmonitor_daemon_state *state)
{
	struct hashmap_iter iter;
	struct fsmonitor_cookie_item *cookie;
	int nr_aborted = 0;

	pthread_mutex_lock(&state->main_lock);

	hashmap_for_each_entry(&state->cookies, &iter, cookie, entry) {
		cookie->result = FCIR_ABORT;
		nr_aborted++;
	}
	
	if (nr_aborted)
		pthread_cond_broadcast(&state->cookies_cond);

	pthread_mutex_unlock(&state->main_lock);
}

/*
 * Requests to and from a FSMonitor Protocol V2 provider use an opaque
 * "token" as a virtual timestamp.  Clients can request a summary of all
 * created/deleted/modified files relative to a token.  In the response,
 * clients receive a new token for the next (relative) request.
 *
 *
 * Token Format
 * ============
 *
 * The contents of the token are private and provider-specific.
 *
 * For the internal fsmonitor--daemon, we define a token as follows:
 *
 *     ":internal:" <session_id> ":" <sequence_nr>
 *
 * The <session_id> is an arbitrary OPAQUE string, such as a GUID,
 * UUID, or {timestamp,pid}.  It is used to group all file system
 * events that happened during the lifespan of an instance of the
 * daemon.
 *
 *     Unlike FSMonitor Protocol V1, it is not defined as a timestamp
 *     and does not define less-than/greater-than relationships.
 *     (There are too many race conditions to rely on file system
 *     event timestamps.)
 *
 * The <sequence_nr> is a simple integer incremented for each file
 * system event received.  When a new <session_id> is created, the
 * <sequence_nr> is reset.
 *
 *
 * About Session Ids
 * =================
 *
 * A new <session_id> is created each time the daemon is started.
 *
 * Token Reset(1): A new <session_id> is also created if the daemon
 * loses sync with the file system notification mechanism.
 *
 * Token Reset(2): A new <session_id> MIGHT also be created when
 * complex file system operations are performed.  For example, a
 * directory rename/move/delete sequence that implicitly affects
 * tracked files within.
 *
 * When the daemon resets the token <session_id>, it is free to
 * discard all cached file system events.  By construction, a token
 * reset means that the daemon cannot completely represent the set of
 * file system changes and therefore cannot make any assurances to the
 * client.
 *
 * Clients that present a token with a stale (non-current)
 * <session_id> will always be given a trivial response.
 */
struct fsmonitor_token_data *fsmonitor_new_token_data(void)
{
	static int test_env_value = -1;
	struct fsmonitor_token_data *token;

	token = (struct fsmonitor_token_data *)xcalloc(1, sizeof(*token));

	strbuf_init(&token->token_sid, 0);
	token->queue_head = NULL;
	token->queue_tail = NULL;
	token->client_ref_count = 0;

	if (test_env_value < 0)
		test_env_value = git_env_bool("GIT_TEST_FSMONITOR_SID", 0);

	if (!test_env_value) {
		struct timeval tv;
		struct tm tm;
		time_t secs;

		gettimeofday(&tv, NULL);
		secs = tv.tv_sec;
		gmtime_r(&secs, &tm);

		strbuf_addf(&token->token_sid,
			    "%d.%4d%02d%02dT%02d%02d%02d.%06ldZ",
			    getpid(),
			    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			    tm.tm_hour, tm.tm_min, tm.tm_sec,
			    (long)tv.tv_usec);
	} else {
		strbuf_addf(&token->token_sid, "test_%08x", test_env_value++);
	}

	return token;
}

void fsmonitor_free_token_data(struct fsmonitor_token_data *token)
{
	if (!token)
		return;

	assert(token->client_ref_count == 0);

	strbuf_release(&token->token_sid);

	while (token->queue_head) {
		struct fsmonitor_queue_item *next = token->queue_head->next;
		free(token->queue_head);
		token->queue_head = next;
	}

	token->queue_tail = NULL;

	free(token);
}

/*
 * Flush all of our cached data about the filesystem.  Call this if we
 * lose sync with the filesystem and miss some notification events.
 *
 * [1] If we are missing events, then we no longer have a complete
 *     history of the directory (relative to our current start token).
 *     We should create a new token and start fresh (as if we just
 *     booted up).
 *
 * [2] Some of those lost events may have been for cookie files.  We
 *     should assume the worst and abort them rather letting them starve.
 *
 * If there are no readers of the the current token data series, we
 * can free it now.  Otherwise, let the last reader free it.  Either
 * way, the old token data series is no longer associated with our
 * state data.
 */
void fsmonitor_force_resync(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_token_data *free_me = NULL;
	struct fsmonitor_token_data *new_one = NULL;

	trace2_data_string("fsmonitor", NULL, "fsm-listen", "overflow");

	fsmonitor_cookie_abort_all(state);

	new_one = fsmonitor_new_token_data();

	pthread_mutex_lock(&state->main_lock);
	if (state->current_token_data->client_ref_count == 0)
		free_me = state->current_token_data;
	state->current_token_data = new_one;
	pthread_mutex_unlock(&state->main_lock);

	if (free_me)
		fsmonitor_free_token_data(free_me);
}

/*
 * Format an opaque token string to send to the client.
 */
static void fsmonitor_format_response_token(
	struct strbuf *response_token,
	const struct strbuf *response_sid,
	const struct fsmonitor_queue_item *queue_item)
{
	uint64_t seq_nr = (queue_item) ? queue_item->token_seq_nr + 1: 0;

	strbuf_reset(response_token);
	strbuf_addf(response_token, ":internal:%s:%"PRIu64,
		    response_sid->buf, seq_nr);
}

/*
 * Parse an opaque token from the client.
 */
static int fsmonitor_parse_client_token(const char *buf_token,
					struct strbuf *requested_sid,
					uint64_t *seq_nr)
{
	const char *p;
	char *p_end;

	strbuf_reset(requested_sid);
	*seq_nr = 0;

	if (!skip_prefix(buf_token, ":internal:", &p))
		return 1;

	while (*p && *p != ':')
		strbuf_addch(requested_sid, *p++);
	if (!*p++)
		return 1;

	*seq_nr = (uint64_t)strtoumax(p, &p_end, 10);
	if (*p_end)
		return 1;

	return 0;
}

KHASH_INIT(str, const char *, int, 0, kh_str_hash_func, kh_str_hash_equal);

static ipc_server_application_cb handle_client;

static int handle_client(void *data, const char *command,
			 ipc_server_reply_cb *reply,
			 struct ipc_server_reply_data *reply_data)
{
	struct fsmonitor_daemon_state *state = data;
	struct fsmonitor_token_data *token_data = NULL;
	struct strbuf response_token = STRBUF_INIT;
	struct strbuf requested_sid = STRBUF_INIT;
	uintmax_t requested_oldest_seq_nr = 0;
	const char *p;
	const struct fsmonitor_queue_item *queue;
	intmax_t count = 0, duplicates = 0;
	kh_str_t *shown;
	int hash_ret;
	int result;
	int should_free_token_data;
	enum fsmonitor_cookie_item_result cookie_result;

	trace2_data_string("fsmonitor", the_repository, "command", command);

	/*
	 * We expect `command` to be of the form:
	 *
	 * <command> := quit NUL
	 *            | flush NUL
	 *            | <V1-time-since-epoch-ns> NUL
	 *            | <V2-opaque-fsmonitor-token> NUL
	 */

	if (!strcmp(command, "quit")) {
		/*
		 * A client has requested over the socket/pipe that the
		 * daemon shutdown.
		 *
		 * Tell the IPC thread pool to shutdown (which completes
		 * the await in the main thread (which can stop the
		 * fsmonitor listener thread)).
		 */
		return SIMPLE_IPC_QUIT;
	}

	if (!strcmp(command, "flush")) {
		/*
		 * Tell the listener thread to fake a token-resync and
		 * flush everything we have cached (just like if lost
		 * sync with the filesystem).
		 *
		 * Wait here until it has installed a new token-data
		 * into our state.
		 *
		 * Then send a trivial response using the new token.
		 */
		const struct fsmonitor_token_data *token_data;

		pthread_mutex_lock(&state->main_lock);
		token_data = state->current_token_data;
		fsmonitor_listen__request_flush(state);
		while (token_data == state->current_token_data)
			pthread_cond_wait(&state->flush_cond,
					  &state->main_lock);
		pthread_mutex_unlock(&state->main_lock);

		result = 0;
		goto send_trivial_response;
	}

	trace2_region_enter("fsmonitor", "handle_client", the_repository);

	if (!skip_prefix(command, ":internal:", &p)) {
		/* assume V1 timestamp or garbage */

		char *p_end;

		strtoumax(command, &p_end, 10);
		error((*p_end) ?
		      _("fsmonitor: invalid command line '%s'") :
		      _("fsmonitor: unsupported V1 protocol '%s'"),
		      command);
		result = -1;
		goto send_trivial_response;
	}

	/* try V2 token */

	pthread_mutex_lock(&state->main_lock);

	if (fsmonitor_parse_client_token(command, &requested_sid,
					 &requested_oldest_seq_nr)) {
		error(_("fsmonitor: invalid V2 protocol token '%s'"),
		      command);
		pthread_mutex_unlock(&state->main_lock);
		result = -1;
		goto send_trivial_response;
	}
	if (!state->current_token_data) {
		/*
		 * We don't have a token.  This means that the listener
		 * thread has not yet started.
		 */
		pthread_mutex_unlock(&state->main_lock);
		result = 0;
		goto send_trivial_response;
	}
	if (strcmp(requested_sid.buf,
		   state->current_token_data->token_sid.buf)) {
		/*
		 * The client last spoke to a different daemon
		 * instance -OR- the daemon had to resync with
		 * the filesystem (and lost events), so reject.
		 */
		pthread_mutex_unlock(&state->main_lock);
		result = 0;
		trace2_data_string("fsmonitor", the_repository,
				   "serve.token", "different");
		goto send_trivial_response;
	}
	if (!state->current_token_data->queue_tail) {
		/*
		 * The listener has not received any filesystem
		 * events yet.
		 */
		pthread_mutex_unlock(&state->main_lock);
		result = 0;
		goto send_trivial_response;
	}
	if (requested_oldest_seq_nr <
	    state->current_token_data->queue_tail->token_seq_nr) {
		/*
		 * The client wants older events that we have for
		 * this token-sid.  This probably means that we have
		 * flushed older events and now have a coverage gap.
		 */
		error(_("fsmonitor: token gap '%"PRIu64"' '%"PRIu64"'"),
		      requested_oldest_seq_nr,
		      state->current_token_data->queue_tail->token_seq_nr);
		pthread_mutex_unlock(&state->main_lock);
		result = 0;
		goto send_trivial_response;
	}

	pthread_mutex_unlock(&state->main_lock);

	/*
	 * Write a cookie file inside the directory being watched in an
	 * effort to flush out existing filesystem events that we actually
	 * care about.  Suspend this client thread until we see the filesystem
	 * events for this cookie file.
	 */
	cookie_result = fsmonitor_wait_for_cookie(state);
	if (cookie_result != FCIR_SEEN) {
		error(_("fsmonitor: cookie_result '%d' != SEEN"),
		      cookie_result);
		result = 0;
		goto send_trivial_response;
	}

	pthread_mutex_lock(&state->main_lock);

	if (strcmp(requested_sid.buf,
		   state->current_token_data->token_sid.buf)) {
		/*
		 * Ack! The listener thread lost sync with the filesystem
		 * and created a new token while we were waiting for the
		 * cookie file to be created!  Just give up.
		 */
		error(_("fsmonitor: lost filesystem sync '%s' '%s'"),
		      requested_sid.buf,
		      state->current_token_data->token_sid.buf);
		pthread_mutex_unlock(&state->main_lock);
		result = 0;
		goto send_trivial_response;
	}

	/*
	 * We're going to hold onto a pointer to the current
	 * token-data while we walk the contents of the queue.
	 * During this time, we will NOT be under the lock.
	 * So we ref-count it.
	 *
	 * This allows the listener thread to continue prepending
	 * new queue-items to the token-data (which we'll ignore).
	 *
	 * AND it allows the listener thread to do a token-reset
	 * (and install a new `current_token_data`).
	 */
	token_data = state->current_token_data;
	token_data->client_ref_count++;

	queue = token_data->queue_head;

	/*
	 * FSMonitor Protocol V2 requires that we send a response header
	 * with a "current token" and then all of the paths that changed
	 * since the "requested token".
	 *
	 * TODO Note that this design is somewhat broken.  The listener
	 * TODO listener thread is continuously updating the queue of
	 * TODO items (prepending them to the list), so by the time we
	 * TODO send the current list to the client, there may be more
	 * TODO items present.  Granted, the request "everything since t0"
	 * TODO is a bit arbitrary for a filesystem in motion.  Do we want
	 * TODO to consider building the response path list in memory and
	 * TODO loop-back to pick up any "extremely fresh" path items
	 * TODO before building the response header and actually sending
	 * TODO the response?
	 */
	fsmonitor_format_response_token(&response_token,
					&token_data->token_sid,
					token_data->queue_head);

	pthread_mutex_unlock(&state->main_lock);

	reply(reply_data, response_token.buf, response_token.len + 1);
	trace2_data_string("fsmonitor", the_repository, "serve.token",
			   response_token.buf);

	shown = kh_init_str();
	while (queue && queue->token_seq_nr >= requested_oldest_seq_nr) {
		if (kh_get_str(shown, queue->interned_path) != kh_end(shown))
			duplicates++;
		else {
			kh_put_str(shown, queue->interned_path, &hash_ret);

			// TODO This loop is writing 1 pathname at a time.
			// TODO This causes a pkt-line write per file.
			// TODO This will cause a context switch as the client
			// TODO will try to do a pkt-line read.
			// TODO We should consider sending a batch in a
			// TODO large buffer.

			/* write the path, followed by a NUL */
			if (reply(reply_data,
				  queue->interned_path, strlen(queue->interned_path) + 1) < 0)
				break;

			// TODO perhaps guard this with a verbose setting?

			trace2_data_string("fsmonitor", the_repository,
					   "serve.path", queue->interned_path);
			count++;
		}
		queue = queue->next;
	}

	kh_release_str(shown);

	pthread_mutex_lock(&state->main_lock);
	if (token_data->client_ref_count > 0)
		token_data->client_ref_count--;
	should_free_token_data = (token_data->client_ref_count == 0 &&
				  token_data != state->current_token_data);
	pthread_mutex_unlock(&state->main_lock);
	/*
	 * If the listener thread did a token-reset and installed a new
	 * token-data AND we are the last reader of this token-data, then
	 * we need to free it.
	 */
	if (should_free_token_data)
		fsmonitor_free_token_data(token_data);

	trace2_data_intmax("fsmonitor", the_repository, "serve.count", count);
	trace2_data_intmax("fsmonitor", the_repository, "serve.skipped-duplicates", duplicates);

	strbuf_release(&response_token);
	strbuf_release(&requested_sid);

	trace2_region_leave("fsmonitor", "handle_client", the_repository);
	return 0;

send_trivial_response:
	pthread_mutex_lock(&state->main_lock);
	fsmonitor_format_response_token(&response_token,
					&state->current_token_data->token_sid,
					state->current_token_data->queue_head);
	pthread_mutex_unlock(&state->main_lock);

	reply(reply_data, response_token.buf, response_token.len + 1);
	trace2_data_string("fsmonitor", the_repository, "serve.token",
			   response_token.buf);
	reply(reply_data, "/", 2);
	trace2_data_intmax("fsmonitor", the_repository, "serve.trivial", 1);

	strbuf_release(&response_token);
	strbuf_release(&requested_sid);

	trace2_region_leave("fsmonitor", "handle_client", the_repository);
	return result;
}

enum fsmonitor_path_type fsmonitor_classify_path(const char *path, size_t len)
{
	if (len < 4 || fspathncmp(path, ".git", 4) || (path[4] && path[4] != '/'))
		return IS_WORKTREE_PATH;

	if (len == 4 || len == 5)
		return IS_DOT_GIT;

	if (len > 4 && starts_with(path + 5, FSMONITOR_COOKIE_PREFIX))
		return IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX;

	return IS_INSIDE_DOT_GIT;
}

static int cookies_cmp(const void *data, const struct hashmap_entry *he1,
		     const struct hashmap_entry *he2, const void *keydata)
{
	const struct fsmonitor_cookie_item *a =
		container_of(he1, const struct fsmonitor_cookie_item, entry);
	const struct fsmonitor_cookie_item *b =
		container_of(he2, const struct fsmonitor_cookie_item, entry);

	return strcmp(a->name, keydata ? keydata : b->name);
}

struct fsmonitor_queue_item *fsmonitor_private_add_path(
	struct fsmonitor_queue_item *queue_head,
	const char *path, uint64_t token_seq_nr)
{
	struct fsmonitor_queue_item *item;

	if (queue_head) {
		/*
		 * The queue/list is implicitly sorted (by construction)
		 * from newest to oldest.
		 */
		assert(token_seq_nr > queue_head->token_seq_nr);
	}

	// TODO maybe only emit this when verbose
	trace2_data_string("fsmonitor", the_repository, "path", path);

	item = xmalloc(sizeof(*item));
	item->interned_path = strintern(path);
	item->token_seq_nr = token_seq_nr;
	item->next = queue_head;

	return item;
}

void fsmonitor_publish_queue_paths(
	struct fsmonitor_daemon_state *state,
	struct fsmonitor_queue_item *queue_head,
	struct fsmonitor_queue_item *queue_tail)
{
	if (!queue_head)
		return;

	assert(queue_tail);

	pthread_mutex_lock(&state->main_lock);
	if (state->current_token_data->queue_head) {
		/*
		 * The queue/list is implicitly sorted (by construction)
		 * from newest to oldest.  And the items in the given
		 * fragment are newer than anything already in the queue.
		 */
		assert(queue_tail->token_seq_nr >
		       state->current_token_data->queue_head->token_seq_nr);
	}

	queue_tail->next = state->current_token_data->queue_head;
	state->current_token_data->queue_head = queue_head;

	if (!state->current_token_data->queue_tail)
		state->current_token_data->queue_tail = queue_tail;

	pthread_mutex_unlock(&state->main_lock);
}

static void *fsmonitor_listen_thread_proc(void *_state)
{
	struct fsmonitor_daemon_state *state = _state;

	trace2_thread_start("fsm-listen");

	pthread_mutex_lock(&state->main_lock);
	state->current_token_data = fsmonitor_new_token_data();
	pthread_mutex_unlock(&state->main_lock);

	fsmonitor_listen__loop(state);

	pthread_mutex_lock(&state->main_lock);
	if (state->current_token_data &&
	    state->current_token_data->client_ref_count == 0)
		fsmonitor_free_token_data(state->current_token_data);
	state->current_token_data = NULL;
	pthread_mutex_unlock(&state->main_lock);

	trace2_thread_exit();
	return NULL;
}

static int fsmonitor_run_daemon(void)
{
	struct fsmonitor_daemon_state state;

	hashmap_init(&state.cookies, cookies_cmp, NULL, 0);
	pthread_mutex_init(&state.main_lock, NULL);
	pthread_cond_init(&state.cookies_cond, NULL);
	pthread_cond_init(&state.flush_cond, NULL);
	state.error_code = 0;

	if (fsmonitor_listen__ctor(&state))
		return error(_("could not initialize listener thread"));

	/*
	 * Start the IPC thread pool before the we've started the file
	 * system event listener thread so that we have the IPC handle
	 * before we need it.  (We do need to be carefull because
	 * quickly arriving client connection events may cause
	 * handle_client() to get called before we have the fsmonitor
	 * listener thread running.)
	 */
	if (ipc_server_run_async(&state.ipc_server_data,
				 git_path_fsmonitor(),
				 fsmonitor__ipc_threads,
				 handle_client,
				 &state)) {
		pthread_cond_destroy(&state.cookies_cond);
		pthread_cond_destroy(&state.flush_cond);
		pthread_mutex_destroy(&state.main_lock);
		fsmonitor_listen__dtor(&state);

		return error(_("could not start IPC thread pool"));
	}

	/*
	 * Start the fsmonitor listener thread to collect filesystem
	 * events.
	 */
	if (pthread_create(&state.watcher_thread, NULL,
			   fsmonitor_listen_thread_proc, &state) < 0) {
		ipc_server_stop_async(state.ipc_server_data);
		ipc_server_await(state.ipc_server_data);
		ipc_server_free(state.ipc_server_data);

		pthread_cond_destroy(&state.cookies_cond);
		pthread_cond_destroy(&state.flush_cond);
		pthread_mutex_destroy(&state.main_lock);
		fsmonitor_listen__dtor(&state);

		return error(_("could not start fsmonitor listener thread"));
	}

	/*
	 * The daemon is now fully functional in background threads.
	 * Wait for the IPC thread pool to shutdown (whether by client
	 * request or from filesystem activity).
	 */
	ipc_server_await(state.ipc_server_data);

	/*
	 * The fsmonitor listener thread may have received a shutdown
	 * event from the IPC thread pool, but it doesn't hurt to tell
	 * it again.  And wait for it to shutdown.
	 */
	fsmonitor_listen__stop_async(&state);
	pthread_join(state.watcher_thread, NULL);

	ipc_server_free(state.ipc_server_data);

	pthread_cond_destroy(&state.cookies_cond);
	pthread_cond_destroy(&state.flush_cond);
	pthread_mutex_destroy(&state.main_lock);
	fsmonitor_listen__dtor(&state);

	return state.error_code;
}

/*
 * If the daemon is running (in another process), ask it to quit and
 * wait for it to stop.
 */
static int fsmonitor_stop_daemon(void)
{
	struct strbuf answer = STRBUF_INIT;
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;
	int ret;
	int fd;
	enum ipc_active_state state;

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;

	state = ipc_client_try_connect(git_path_fsmonitor(), &options, &fd);
	if (state != IPC_STATE__LISTENING) {
		die("daemon not running");
		return -1;
	}

	ret = ipc_client_send_command_to_fd(fd, "quit", &answer);
	strbuf_release(&answer);
	close(fd);

	if (ret == -1) {
		die("could sent quit command to daemon");
		return -1;
	}

	// TODO Should we get rid of this polling loop and just return
	// TODO after sending the quit command?

	trace2_region_enter("fsmonitor", "polling-for-daemon-exit", NULL);
	while (fsmonitor_daemon_is_running())
		sleep_millisec(50);
	trace2_region_leave("fsmonitor", "polling-for-daemon-exit", NULL);

	return 0;
}

/*
 * Tell the daemon to flush its cache.  This is primarily a test
 * feature used to simulate a loss of sync with the filesystem where
 * we miss events.
 */
static int fsmonitor_flush_daemon(void)
{
	struct strbuf answer = STRBUF_INIT;
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;
	int ret;
	int fd;
	enum ipc_active_state state;

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;

	state = ipc_client_try_connect(git_path_fsmonitor(), &options, &fd);
	if (state != IPC_STATE__LISTENING) {
		die("daemon not running");
		return -1;
	}

	ret = ipc_client_send_command_to_fd(fd, "flush", &answer);
	close(fd);

	if (ret == -1) {
		die("could sent flush command to daemon");
		return -1;
	}

	write_in_full(1, answer.buf, answer.len);
	strbuf_release(&answer);

	return 0;
}

#endif

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	enum daemon_mode {
		QUERY = 0, START, RUN, STOP, FLUSH, IS_RUNNING, IS_SUPPORTED
	} mode = QUERY;
	struct option options[] = {
		OPT_CMDMODE(0, "start", &mode, N_("run in the background"),
			    START),
		OPT_CMDMODE(0, "run", &mode,
			    N_("run the daemon in the foreground"), RUN),
		OPT_CMDMODE(0, "stop", &mode, N_("stop the running daemon"),
			    STOP),
		OPT_CMDMODE(0, "query", &mode,
			    N_("query the daemon (starting if necessary)"),
			    QUERY),
		OPT_CMDMODE(0, "flush", &mode, N_("flush cached filesystem events"),
			    FLUSH),

		OPT_CMDMODE('t', "is-running", &mode,
			    N_("test whether the daemon is running"),
			    IS_RUNNING),
		OPT_CMDMODE(0, "is-supported", &mode,
			    N_("determine internal fsmonitor on this platform"),
			    IS_SUPPORTED),

		OPT_INTEGER(0, "ipc-threads",
			    &fsmonitor__ipc_threads,
			    N_("use <n> ipc threads")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	git_config(fsmonitor_config, NULL);

	argc = parse_options(argc, argv, prefix, options,
			     builtin_fsmonitor__daemon_usage, 0);
	if (fsmonitor__ipc_threads < 1)
		die(_("invalid 'ipc-threads' value (%d)"),
		    fsmonitor__ipc_threads);

	if (mode == QUERY) {
		/*
		 * Commands of the form `fsmonitor--daemon --query <token>`
		 * cause this instance to behave as a CLIENT.  We connect to
		 * the existing daemon process and ask it for the cached data.
		 * We start a new daemon process in the background if one is
		 * not already running.  In both cases, we leave the background
		 * daemon running after we exit.
		 *
		 * This feature is primarily used by the test suite.
		 *
		 * <token> is an opaque "fsmonitor V2" token.
		 */
		struct strbuf answer = STRBUF_INIT;
		int ret;

		if (argc != 1)
			usage_with_options(builtin_fsmonitor__daemon_usage,
					   options);

		ret = fsmonitor_query_daemon(argv[0], &answer);
		if (ret < 0)
			die(_("could not query fsmonitor daemon"));

		write_in_full(1, answer.buf, answer.len);
		strbuf_release(&answer);

		return 0;
	}

	if (argc != 0)
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	if (mode == IS_SUPPORTED)
		return !FSMONITOR_DAEMON_IS_SUPPORTED;

	if (mode == IS_RUNNING)
		return !fsmonitor_daemon_is_running();

	if (mode == STOP)
		return !!fsmonitor_stop_daemon();

	if (mode == FLUSH)
		return !!fsmonitor_flush_daemon();

	// TODO Rather than explicitly testing whether the daemon is already running,
	// TODO just try to gently create a new daemon and handle the error code.
	// TODO (If we don't want to do that, please add a trace region around this
	// TODO call so that we know why we are probing the daemon.)

	if (fsmonitor_daemon_is_running())
		die("fsmonitor daemon is already running.");

	if (mode == START) {
#ifdef GIT_WINDOWS_NATIVE
		/*
		 * Windows cannot daemonize(); emulate it.
		 */
		return !!fsmonitor_spawn_daemon();
#else
		/*
		 * Run the daemon in the process of the child created
		 * by fork() since only the child returns from daemonize().
		 */
		if (daemonize())
			BUG(_("daemonize() not supported on this platform"));
		return !!fsmonitor_run_daemon();
#endif
	}

	if (mode == RUN)
		return !!fsmonitor_run_daemon();

	BUG(_("Unhandled command mode %d"), mode);
}

// TODO BIG PICTURE QUESTION:
// TODO Is there an inherent race condition in this whole thing?
// TODO
// TODO I'm wondering if the client should create the cookie file
// TODO and then ask for everything from a given timestamp UPTO AND
// TODO the cookie file event.
// TODO  [1] This would remove some of the randomness WRT the
// TODO      client and the last event reported.
// TODO  [2] The client would be responsible for creating and deleting
// TODO      the cookie file -- so the daemon would not need write
// TODO      access to the repo.
// TODO  [3] The cookie file creation event could be arriving WHILE
// TODO      connection is established.
// TODO  [4] The client could decide the timeout (and just hang up).
//
