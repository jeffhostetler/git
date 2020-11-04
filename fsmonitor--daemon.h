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
struct fsmonitor_path {
	struct hashmap_entry entry;
	const char *path;
};

struct fsmonitor_queue_item {
	struct fsmonitor_path *path;
	uint64_t time;
	struct fsmonitor_queue_item *previous, *next;
};

struct fsmonitor_cookie_item {
	struct hashmap_entry entry;
	const char *name;
	pthread_mutex_t seen_lock;
	pthread_cond_t seen_cond;
	int seen;
};

#define FSMONITOR_COOKIE_PREFIX ".watchman-cookie-git-"

struct fsmonitor_daemon_backend_data;

struct fsmonitor_daemon_state {
	struct hashmap paths;
	struct fsmonitor_queue_item *first;
	struct fsmonitor_queue_item *last;
	uint64_t latest_update;
	pthread_t watcher_thread;
	pthread_mutex_t queue_update_lock, cookies_lock;
	int cookie_seq;
	struct hashmap cookies;
	struct string_list cookie_list;
	int error_code;
	struct fsmonitor_daemon_backend_data *backend_data;

	struct ipc_server_data *ipc_server_data;
};

void fsmonitor_cookie_seen_trigger(struct fsmonitor_daemon_state *state,
				   const char *cookie_name);

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
 */
int fsmonitor_queue_path(struct fsmonitor_daemon_state *state,
			 struct fsmonitor_queue_item **queue,
			 const char *path, size_t len, uint64_t time);

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
