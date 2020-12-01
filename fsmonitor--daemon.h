#ifndef FSMONITOR_DAEMON_H
#define FSMONITOR_DAEMON_H

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND

#include "cache.h"
#include "dir.h"
#include "run-command.h"
#include "simple-ipc.h"
#include "thread-utils.h"

extern const char *git_path_fsmonitor(void);

/*
 * Connect to the fsmonitor daemon process (spawn it if necessary)
 * and ask for the set of changed files since the given token.
 */
int fsmonitor_daemon__send_query_command(
	const char *since_token,
	struct strbuf *answer);

int fsmonitor_spawn_daemon(void);

/* Internal fsmonitor */

struct fsmonitor_batch;

/*
 * Create a new batch of path(s).  The returned batch is considered
 * private and not linked into the fsmonitor daemon state.  The caller
 * should fill this batch with one or more paths and then publish it.
 */
struct fsmonitor_batch *fsmonitor_batch__new(void);

/*
 * Free this batch and return the value of the batch->next field.
 */
struct fsmonitor_batch *fsmonitor_batch__free(struct fsmonitor_batch *batch);

/*
 * Add this path to this batch of modified files.
 *
 * The batch should be private and NOT (yet) linked into the fsmonitor
 * daemon state and therefore not yet visible to worker threads and so
 * no locking is required.
 */
void fsmonitor_batch__add_path(struct fsmonitor_batch *batch, const char *path);

struct fsmonitor_token_data {
	struct strbuf token_id;
	struct fsmonitor_batch *batch_head;
	struct fsmonitor_batch *batch_tail;
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
	pthread_t listener_thread;
	pthread_mutex_t main_lock;

	struct fsmonitor_token_data *current_token_data;

	pthread_cond_t cookies_cond;
	int cookie_seq;
	struct hashmap cookies;

	int error_code;
	struct fsmonitor_daemon_backend_data *backend_data;

	struct ipc_server_data *ipc_server_data;

	int test_client_delay_ms;
};

enum fsmonitor_path_type {
	IS_WORKTREE_PATH = 0,
	IS_DOT_GIT,
	IS_INSIDE_DOT_GIT,
	IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX,
};

enum fsmonitor_path_type fsmonitor_classify_path(const char *path, size_t len);

#define FSMONITOR_DAEMON_QUIT -2

/*
 * Prepend the this batch of path(s) onto the list of batches associated
 * with the current token.  This makes the batch visible to worker threads.
 *
 * The caller no longer owns the batch and must not free it.
 *
 * Wake up the client threads waiting on these cookies.
 */
void fsmonitor_publish(struct fsmonitor_daemon_state *state,
		       struct fsmonitor_batch *batch,
		       const struct string_list *cookie_names);

/*
 * If the platform-specific layer loses sync with the filesystem,
 * it should call this to invalidate cached data and abort waiting
 * threads.
 */
void fsmonitor_force_resync(struct fsmonitor_daemon_state *state);

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
