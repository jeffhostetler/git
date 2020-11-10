#ifndef FSMONITOR_DAEMON_H
#define FSMONTIOR_DAEMON_H

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND

#include "cache.h"
#include "dir.h"
#include "run-command.h"
#include "simple-ipc.h"
#include "thread-utils.h"

extern const char *git_path_fsmonitor(void);

int fsmonitor_query_daemon(const char *since, struct strbuf *answer);

// TODO deprecate the _is_running() api.  use either the _get_active_state()
// TODO or the ipc _try_connect.  because (as described in simple-ipc.h)
// TODO this is just a guess based on the existence of the named pipe or
// TODO UDS socket.
int fsmonitor_daemon_is_running(void);
int fsmonitor_spawn_daemon(void);

enum ipc_active_state fsmonitor_daemon_get_active_state(void);

/* Internal fsmonitor */

struct fsmonitor_queue_item {
	struct fsmonitor_queue_item *next;
	const char *interned_path; /* see strintern() */
	uint64_t token_seq_nr;
};

struct fsmonitor_token_data {
	struct strbuf token_sid;
	struct fsmonitor_queue_item *queue_head;
	struct fsmonitor_queue_item *queue_tail;
	uint64_t client_ref_count;
};

enum fsmonitor_cookie_item_result {
	FCIR_ERROR = -1, /* could not create cookie file ? */
	FCIR_INIT = 0,
	FCIR_SEEN,
	FCIR_ABORT,
};

struct fsmonitor_cookie_item {
	struct hashmap_entry entry;
	const char *name;
	enum fsmonitor_cookie_item_result result;
};

#define FSMONITOR_COOKIE_PREFIX ".watchman-cookie-git-"

struct fsmonitor_daemon_backend_data; /* opaque platform-specific data */

struct fsmonitor_daemon_state {
	pthread_t watcher_thread;
	pthread_mutex_t main_lock;

	struct fsmonitor_token_data *current_token_data;

	pthread_cond_t cookies_cond;
	int cookie_seq;
	struct hashmap cookies;

	int error_code;
	struct fsmonitor_daemon_backend_data *backend_data;

	struct ipc_server_data *ipc_server_data;
};

/*
 * Mark these cookies as _SEEN and wake up the corresponding client threads.
 */
void fsmonitor_cookie_mark_seen(struct fsmonitor_daemon_state *state,
				const struct string_list *cookie_names);

/*
 * Set _ABORT on all pending cookies and wake up all client threads.
 */
void fsmonitor_cookie_abort_all(struct fsmonitor_daemon_state *state);

enum fsmonitor_path_type {
	IS_WORKTREE_PATH = 0,
	IS_DOT_GIT,
	IS_INSIDE_DOT_GIT,
	IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX,
};

enum fsmonitor_path_type fsmonitor_classify_path(const char *path, size_t len);

#define FSMONITOR_DAEMON_QUIT -2

/*
 * Register a path as having been touched at a certain time.
 *
 * Create a new item for the given (path, time) and prepend it
 * to the given queue.  This is a private list of items and NOT
 * (yet) linked into the fsmonitor_daemon_state (and therefore
 * not yet visible to worker threads), so no locking is required.
 *
 * Returns the new head of the list.
 */
struct fsmonitor_queue_item *fsmonitor_private_add_path(
	struct fsmonitor_queue_item *queue_head,
	const char *path, uint64_t time);

/*
 * Link the given private item queue into the official
 * fsmonitor_daemon_state and thus make them visible to
 * worker threads.
 */
void fsmonitor_publish_queue_paths(
	struct fsmonitor_daemon_state *state,
	struct fsmonitor_queue_item *queue_head,
	struct fsmonitor_queue_item *queue_tail);

/*
 * Create a new token_data.  This creates a new unique ID, such as a
 * GUID or timestamp, that can be included in a FSMonitor protocol V2
 * message to let us know if the current client is talking to the
 * same instance of the daemon and that we have a contiguous range of
 * file system notification events.
 */
struct fsmonitor_token_data *fsmonitor_new_token_data(void);
void fsmonitor_free_token_data(struct fsmonitor_token_data *token);

/* This needs to be implemented by the backend */

/*
 * Initialize platform-specific data for the fsmonitor listener thread.
 * This will be called from the main thread PRIOR to staring the listener.
 */
int fsmonitor_listen__ctor(struct fsmonitor_daemon_state *state);

/*
 * Cleanup platform-specific data for the fsmonitor listener thread.
 * This will be called from the main thread AFTER joining the listener.
 */
void fsmonitor_listen__dtor(struct fsmonitor_daemon_state *state);

/*
 * The main body of the platform-specific fsmonitor listener thread to
 * watch for filesystem events.  This will be called inside the
 * fsmonitor thread.
 *
 * It should call `ipc_server_stop_async()` if the listener thread
 * prematurely terminates (because of a filesystem error or if it
 * detects that the .git directory has been deleted).  (It should NOT
 * do so if the listener thread receives a normal shutdown signal from
 * the IPC layer.)
 *
 * It should set `state->error_code` to -1 if the daemon should exit
 * with an error.
 */
void fsmonitor_listen__loop(struct fsmonitor_daemon_state *state);

/*
 * Gently request that the fsmonitor listener thread shutdown.
 * It does not wait for it to stop.  The caller should do a JOIN
 * to wait for it.
 */
void fsmonitor_listen__stop_async(struct fsmonitor_daemon_state *state);

#endif /* HAVE_FSMONITOR_DAEMON_BACKEND */
#endif /* FSMONITOR_DAEMON_H */
