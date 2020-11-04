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
	volatile int my_cookie_seq;

	pthread_mutex_lock(&state->cookies_lock);
	my_cookie_seq = state->cookie_seq++;
	pthread_mutex_unlock(&state->cookies_lock);

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

	pthread_mutex_lock(&state->cookies_lock);
	hashmap_add(&state->cookies, &cookie.entry);
	pthread_mutex_unlock(&state->cookies_lock);

	cookie_path = git_pathdup("%s", cookie.name);
	fd = open(cookie_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) {
		close(fd);
		unlink_or_warn(cookie_path);

		// TODO We need a way to timeout this loop in case the
		// TODO listener thread never sees the event for the
		// TODO cookie file.

		pthread_mutex_lock(&state->cookies_lock);
		while (cookie.result == FCIR_INIT)
			pthread_cond_wait(&state->cookies_cond,
					  &state->cookies_lock);

		hashmap_remove(&state->cookies, &cookie.entry, NULL);
		pthread_mutex_unlock(&state->cookies_lock);
	} else {
		pthread_mutex_lock(&state->cookies_lock);
		cookie.result = FCIR_ERROR;
		hashmap_remove(&state->cookies, &cookie.entry, NULL);
		pthread_mutex_unlock(&state->cookies_lock);
	}

	free((char*)cookie.name);
	free(cookie_path);
	return cookie.result;
}

void fsmonitor_cookie_seen_trigger(struct fsmonitor_daemon_state *state,
				   const char *cookie_name)
{
	struct fsmonitor_cookie_item key;
	struct fsmonitor_cookie_item *cookie;

	hashmap_entry_init(&key.entry, strhash(cookie_name));
	key.name = cookie_name;

	pthread_mutex_lock(&state->cookies_lock);
	cookie = hashmap_get_entry(&state->cookies, &key, entry, NULL);
	if (cookie) {
		cookie->result = FCIR_SEEN;
		pthread_cond_broadcast(&state->cookies_cond);
	}
	pthread_mutex_unlock(&state->cookies_lock);
}

KHASH_INIT(str, const char *, int, 0, kh_str_hash_func, kh_str_hash_equal);

static ipc_server_application_cb handle_client;

static int handle_client(void *data, const char *command,
			 ipc_server_reply_cb *reply,
			 struct ipc_server_reply_data *reply_data)
{
	struct fsmonitor_daemon_state *state = data;
	uintmax_t since;
	char *p;
	struct fsmonitor_queue_item *queue;
	struct strbuf token = STRBUF_INIT;
	intmax_t count = 0, duplicates = 0;
	kh_str_t *shown;
	int hash_ret;

	trace2_data_string("fsmonitor", the_repository, "command", command);

	/*
	 * We expect `command` to be of the form:
	 *
	 * <command> := quit NUL
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

	trace2_region_enter("fsmonitor", "serve", the_repository);

	// TODO for now, assume the token is a timestamp.
	// TODO fix this.
	since = strtoumax(command, &p, 10);

	if (*p) {
		error(_("fsmonitor: invalid command line '%s'"), command);
		goto send_trivial_response;
	}

	/*
	 * write out cookie file so the queue gets filled with all
	 * the file system events that happen before the file gets written
	 *
	 * TODO Look at `fsmonitor_cookie_item_result` return value and
	 * TODO consider doing something different if get !SEEN.
	 * TODO For example, if we could not create the cookie file.
	 * TODO Or if we need to change the token-sid and want to abort
	 * TODO and send a trivial result.
	 */
	fsmonitor_wait_for_cookie(state);

	pthread_mutex_lock(&state->queue_update_lock);
	if (since < state->latest_update) {
		volatile uintmax_t latest = state->latest_update;
		pthread_mutex_unlock(&state->queue_update_lock);
		error(_("fsmonitor: incorrect/early timestamp (since %" PRIuMAX")(latest %" PRIuMAX")"),
		      since, latest);
		goto send_trivial_response;
	}

	if (!state->latest_update)
		BUG("latest_update was not updated");

	queue = state->first;
	strbuf_addf(&token, "%"PRIu64"", state->latest_update);
	pthread_mutex_unlock(&state->queue_update_lock);

	reply(reply_data, token.buf, token.len + 1);
	trace2_data_string("fsmonitor", the_repository, "serve.token", token.buf);

	shown = kh_init_str();
	while (queue && queue->time >= since) {
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
	strbuf_release(&token);
	trace2_data_intmax("fsmonitor", the_repository, "serve.count", count);
	trace2_data_intmax("fsmonitor", the_repository, "serve.skipped-duplicates", duplicates);
	trace2_region_leave("fsmonitor", "serve", the_repository);

	return 0;

send_trivial_response:
	pthread_mutex_lock(&state->queue_update_lock);
	strbuf_addf(&token, "%"PRIu64"", state->latest_update);
	pthread_mutex_unlock(&state->queue_update_lock);

	reply(reply_data, token.buf, token.len + 1);
	trace2_data_string("fsmonitor", the_repository, "serve.token", token.buf);
	reply(reply_data, "/", 2);
	trace2_data_intmax("fsmonitor", the_repository, "serve.trivial", 1);

	strbuf_release(&token);
	trace2_region_leave("fsmonitor", "serve", the_repository);

	return -1;
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

int fsmonitor_queue_path(struct fsmonitor_queue_item **queue,
			 const char *path, size_t len, uint64_t time)
{
	struct fsmonitor_queue_item *item;

	// TODO maybe only emit this when verbose
	trace2_data_string("fsmonitor", the_repository, "path", path);

	item = xmalloc(sizeof(*item));
	item->interned_path = strintern(path);
	item->time = time;
	item->previous = NULL;
	item->next = *queue;
	(*queue)->previous = item;
	*queue = item;

	return 0;
}

static void *fsmonitor_listen_thread_proc(void *_state)
{
	struct fsmonitor_daemon_state *state = _state;

	trace2_thread_start("fsm-listen");

	fsmonitor_listen__loop(state);

	trace2_thread_exit();
	return NULL;
}

static int fsmonitor_run_daemon(void)
{
	struct fsmonitor_daemon_state state = {
		.cookie_list = STRING_LIST_INIT_DUP
	};

	hashmap_init(&state.cookies, cookies_cmp, NULL, 0);
	pthread_mutex_init(&state.queue_update_lock, NULL);
	pthread_mutex_init(&state.cookies_lock, NULL);
	pthread_cond_init(&state.cookies_cond, NULL);

	state.error_code = 0;
	state.latest_update = getnanotime();

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
		pthread_mutex_destroy(&state.cookies_lock);
		pthread_mutex_destroy(&state.queue_update_lock);
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
		pthread_mutex_destroy(&state.cookies_lock);
		pthread_mutex_destroy(&state.queue_update_lock);
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
	pthread_mutex_destroy(&state.cookies_lock);
	pthread_mutex_destroy(&state.queue_update_lock);
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
#endif

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	enum daemon_mode {
		QUERY = 0, RUN, START, STOP, IS_RUNNING, IS_SUPPORTED
	} mode = QUERY;
	struct option options[] = {
		OPT_CMDMODE(0, "query", &mode, N_("query the daemon"), QUERY),
		OPT_CMDMODE(0, "run", &mode, N_("run the daemon"), RUN),
		OPT_CMDMODE(0, "start", &mode, N_("run in the background"),
			    START),
		OPT_CMDMODE(0, "stop", &mode, N_("stop the running daemon"),
			    STOP),
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
// TODO The client asks for all changes since a given timestamp.
// TODO The server creates a cookie file and blocks the response
// TODO until it appears.
// TODO  [1] The cookie is created at a random time (WRT the client)
// TODO      (and considering the race for the daemon to accept()
// TODO      the client connection).
// TODO  [2] The fs notify code handles events in batches
// TODO  [3] The response is everything from the requested timestamp
// TODO      thru the end of the batch (another bit of randomness).
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
