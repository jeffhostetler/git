#ifndef FSMONITOR_H
#define FSMONITOR_H

#include "cache.h"
#include "dir.h"
#include "run-command.h"
#include "simple-ipc.h"

extern struct trace_key trace_fsmonitor;

/*
 * Read the fsmonitor index extension and (if configured) restore the
 * CE_FSMONITOR_VALID state.
 */
int read_fsmonitor_extension(struct index_state *istate, const void *data, unsigned long sz);

/*
 * Fill the fsmonitor_dirty ewah bits with their state from the index,
 * before it is split during writing.
 */
void fill_fsmonitor_bitmap(struct index_state *istate);

/*
 * Write the CE_FSMONITOR_VALID state into the fsmonitor index
 * extension.  Reads from the fsmonitor_dirty ewah in the index.
 */
void write_fsmonitor_extension(struct strbuf *sb, struct index_state *istate);

/*
 * Add/remove the fsmonitor index extension
 */
void add_fsmonitor(struct index_state *istate);
void remove_fsmonitor(struct index_state *istate);

/*
 * Add/remove the fsmonitor index extension as necessary based on the current
 * core.fsmonitor setting.
 */
void tweak_fsmonitor(struct index_state *istate);

/*
 * Run the configured fsmonitor integration script and clear the
 * CE_FSMONITOR_VALID bit for any files returned as dirty.  Also invalidate
 * any corresponding untracked cache directory structures. Optimized to only
 * run the first time it is called.
 */
void refresh_fsmonitor(struct index_state *istate);

/*
 * Set the given cache entries CE_FSMONITOR_VALID bit. This should be
 * called any time the cache entry has been updated to reflect the
 * current state of the file on disk.
 */
static inline void mark_fsmonitor_valid(struct index_state *istate, struct cache_entry *ce)
{
	if (core_fsmonitor && !(ce->ce_flags & CE_FSMONITOR_VALID)) {
		istate->cache_changed = 1;
		ce->ce_flags |= CE_FSMONITOR_VALID;
		trace_printf_key(&trace_fsmonitor, "mark_fsmonitor_clean '%s'", ce->name);
	}
}

/*
 * Clear the given cache entry's CE_FSMONITOR_VALID bit and invalidate
 * any corresponding untracked cache directory structures. This should
 * be called any time git creates or modifies a file that should
 * trigger an lstat() or invalidate the untracked cache for the
 * corresponding directory
 */
static inline void mark_fsmonitor_invalid(struct index_state *istate, struct cache_entry *ce)
{
	if (core_fsmonitor) {
		ce->ce_flags &= ~CE_FSMONITOR_VALID;
		untracked_cache_invalidate_path(istate, ce->name, 1);
		trace_printf_key(&trace_fsmonitor, "mark_fsmonitor_invalid '%s'", ce->name);
	}
}

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND
#include "thread-utils.h"

extern const char *git_path_fsmonitor(void);
#define FSMONITOR_VERSION 1ul

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
	size_t len;
	uint64_t time;
	enum {
		PATH_IS_UNSPECIFIED = -1,
		PATH_DOES_NOT_EXIST,
		PATH_IS_FILE,
		PATH_IS_DIRECTORY,
	} mode;
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

/*
 * Handle special paths. Returns
 *
 * - 0 if the path is not special,
 *
 * - >0 if it should not be queued (e.g. because it is inside `.git/`),
 *
 * - FSMONITOR_DAEMON_QUIT if the daemon was asked to quit, and
 *
 * - other negative values in case of error.
 */
int fsmonitor_special_path(struct fsmonitor_daemon_state *state,
			   const char *path, size_t len, int was_deleted);
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

#endif /* FSMONITOR_H */
