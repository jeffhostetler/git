#include "cache.h"
#include "config.h"
#include "fsmonitor.h"
#include "fsmonitor--daemon.h"

struct fsmonitor_daemon_backend_data
{
	HANDLE hDir;

	HANDLE hListener[2];
#define LISTENER_SHUTDOWN 0
#define LISTENER_HAVE_DATA 1

	struct strbuf token_sid;
	uint64_t token_seq_nr;
};

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
static void make_new_session_id(struct strbuf *buf_new_sid)
{
	static int test_env_value = -1;

	strbuf_reset(buf_new_sid);

	if (test_env_value < 0)
		test_env_value = git_env_bool("GIT_TEST_FSMONITOR_SID", 0);

	if (!test_env_value) {
		/*
		 * For now, just use {timestamp,pid} because linking
		 * in a GUID or UUID library just adds complexity that
		 * we don't need right now.
		 */
		struct timeval tv;
		struct tm tm;
		time_t secs;

		gettimeofday(&tv, NULL);
		secs = tv.tv_sec;
		gmtime_r(&secs, &tm);

		strbuf_addf(buf_new_sid, "%d.%4d%02d%02dT%02d%02d%02d.%06ldZ",
			    getpid(),
			    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			    tm.tm_hour, tm.tm_min, tm.tm_sec,
			    (long)tv.tv_usec);
		return;
	}

	strbuf_addf(buf_new_sid, "test_%08x", test_env_value++);
}

#if 0
static void make_new_token(struct strbuf *buf_new_token,
			   const char *sid, uint64_t seq_nr)
{
	strbuf_reset(buf_new_token);
	strbuf_addf(buf_new_token, ":internal:%s:%"PRIu64, sid, seq_nr);
}
#endif

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

// TODO Long paths can be 32k WIDE CHARS, but that may expand into more
// TODO than 32k BYTES if there are any UTF8 multi-byte sequences.
// TODO But this is rather rare, try it with a fixed buffer size first
// TODO and if returns 0 and error ERROR_INSUFFICIENT_BUFFER, do the
// TODO do it again the right way.

static int normalize_path(FILE_NOTIFY_INFORMATION *info, struct strbuf *normalized_path)
{
	/* Convert to UTF-8 */
	int len;

	strbuf_reset(normalized_path);
	strbuf_grow(normalized_path, 32768);
	len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
				  info->FileNameLength / sizeof(WCHAR),
				  normalized_path->buf, strbuf_avail(normalized_path) - 1, NULL, NULL);

	if (len == 0 || len >= 32768 - 1)
		return error("could not convert '%.*S' to UTF-8",
			     (int)(info->FileNameLength / sizeof(WCHAR)),
			     info->FileName);

	strbuf_setlen(normalized_path, len);
	return strbuf_normalize_path(normalized_path);
}

void fsmonitor_listen__stop_async(struct fsmonitor_daemon_state *state)
{
	SetEvent(state->backend_data->hListener[LISTENER_SHUTDOWN]);
}

/*
 * Use OVERLAPPED IO to call ReadDirectoryChangesW() so that we can
 * wait for IO and/or a shutdown event.
 *
 * Returns 0 if successful.
 * Returns -1 on error.
 * Returns FSMONITOR_DAEMON_QUIT if shutdown signaled.
 */
static int read_directory_changes_overlapped(
	struct fsmonitor_daemon_state *state,
	char *buffer,
	size_t buffer_len,
	DWORD *count)
{
	OVERLAPPED overlapped;
	DWORD dwWait;

	memset(&overlapped, 0, sizeof(overlapped));

	ResetEvent(state->backend_data->hListener[LISTENER_HAVE_DATA]);
	overlapped.hEvent = state->backend_data->hListener[LISTENER_HAVE_DATA];

	if (!ReadDirectoryChangesW(state->backend_data->hDir,
				   buffer, buffer_len, TRUE,
				   FILE_NOTIFY_CHANGE_FILE_NAME |
				   FILE_NOTIFY_CHANGE_DIR_NAME |
				   FILE_NOTIFY_CHANGE_ATTRIBUTES |
				   FILE_NOTIFY_CHANGE_SIZE |
				   FILE_NOTIFY_CHANGE_LAST_WRITE |
				   FILE_NOTIFY_CHANGE_CREATION,
				   count, &overlapped, NULL))
		return error("ReadDirectoryChangedW failed [GLE %ld]",
			     GetLastError());

	dwWait = WaitForMultipleObjects(2, state->backend_data->hListener, FALSE, INFINITE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_HAVE_DATA &&
	    GetOverlappedResult(state->backend_data->hDir, &overlapped, count, TRUE))
		return 0;

	CancelIoEx(state->backend_data->hDir, &overlapped);
	GetOverlappedResult(state->backend_data->hDir, &overlapped, count, TRUE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_SHUTDOWN)
		return FSMONITOR_DAEMON_QUIT;

	return error("could not read directory changes");
}

int fsmonitor_listen__ctor(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;
	DWORD desired_access = FILE_LIST_DIRECTORY;
	DWORD share_mode =
		FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE;
	HANDLE dir;

	dir = CreateFileW(L".", desired_access, share_mode, NULL, OPEN_EXISTING,
			  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			  NULL);
	if (dir == INVALID_HANDLE_VALUE)
		return error(_("could not watch '.'"));

	data = xcalloc(1, sizeof(*data));

	data->hDir = dir;

	data->hListener[LISTENER_SHUTDOWN] = CreateEvent(NULL, TRUE, FALSE, NULL);
	data->hListener[LISTENER_HAVE_DATA] = CreateEvent(NULL, TRUE, FALSE, NULL);

	strbuf_init(&data->token_sid, 0);
	make_new_session_id(&data->token_sid);

	state->backend_data = data;
	return 0;
}

void fsmonitor_listen__dtor(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;

	if (!state || !state->backend_data)
		return;

	data = state->backend_data;

	if (data->hListener[LISTENER_SHUTDOWN] != INVALID_HANDLE_VALUE)
		CloseHandle(data->hListener[LISTENER_SHUTDOWN]);
	if (data->hListener[LISTENER_HAVE_DATA] != INVALID_HANDLE_VALUE)
		CloseHandle(data->hListener[LISTENER_HAVE_DATA]);

	if (data->hDir != INVALID_HANDLE_VALUE)
		CloseHandle(data->hDir);

	strbuf_release(&data->token_sid);

	FREE_AND_NULL(state->backend_data);
}

// TODO getnanotime() is broken on Windows.  The very first call in the
// TODO process computes something in microseconds and multiplies the
// TODO result by 1000.  Since the NTFS has 100ns resolution, we can
// TODO accidentally under report.  We should set the initial value of
// TODO `state->latest_update` more precisely.
//
// TODO I think the subsequent calls to getnanotime() in the body of the
// TODO loop are also suspect.  There is an inherent race here, right?
// TODO We are getting the clock before waiting (on both the lock and then
// TODO on the kernel notify event).  The presumption is that the clock
// TODO value will be earlier than the mod time of the first file in the
// TODO buffer returned from ReadDirectoryChangesW() -- but we don't know
// TODO that -- we don't control the batching.  (As I Understand It), the
// TODO kernel maintains an internal buffer (hanging off of our directory
// TODO handle) to collect events between our calls to RDCW(), so after
// TODO calling RDCW() and getting a batch of changes (and emptying the
// TODO kernel buffer) and while we are processing them, any new events
// TODO would be added to the kernel buffer *WHILE WE ARE PROCESSING*
// TODO the previous batch.  So the next time we call RDCW() we will get
// TODO events from *BEFORE* our time stamp.  Right???
//
// TODO Also WRT getnanotime(), it returns a value based upon the system
// TODO clock (such as gettimeofday() or QueryPerformance*()).  If it
// TODO unclear if the system clock is in anyway related to the filesystem
// TODO clock.  I know this sounds odd.  For a local NTFS filesystem, they
// TODO are probably the same, but for a network mount or samba share, the
// TODO FS clock is that of the remote host -- which may have a non-trivial
// TODO amount of clock skew.  I think we should lstat() the first file
// TODO received in the batch and make that the baseline for the iteration
// TODO of the loop.
//
// TODO This listener thread should not release the main thread until it
// TODO has established the `latest_update` baseline.  For example,
// TODO register for FS notifications, create a "startup cookie" and
// TODO wait for it to appear in the notification stream.  Mark that
// TODO as the epoch for our queue and etc.  AND THEN release the main
// TODO thread.
//
// TODO TODO I moved the initialization of `latest_update` to the main
// TODO TODO thread to help simplify startup of this thread and the IPC
// TODO TODO thread pool (and to eliminate the initial_mutex,cond vars.
// TODO TODO I need to revisit what that field means and how the initial
// TODO TODO timestamp should be set.
//
// TODO RDCW() has a note in [1] WRT short-names.  We should understand
// TODO what it means.
// TODO [1] https://docs.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw

void fsmonitor_listen__loop(struct fsmonitor_daemon_state *state)
{
	// TODO Investigate the best buffer size.
	// TODO
	// TODO There is a comment in [1] about a 64k buffer limit *IF*
	// TODO monitoring a directory over the network.  But that would
	// TODO be `char[64k]` not `char[64k*2]`.
	// TODO
	// TODO [1] doesn't list a max for a local directory.
	// TODO
	// TODO For very heavy IO loads with deeply nested pathnames, we might
	// TODO need a larger buffer.

	struct strbuf path = STRBUF_INIT;
	char buffer[65536 * sizeof(wchar_t)], *p;
	DWORD count = 0;
	int i;

	for (;;) {

		// TODO Something about this feels dirty and dangerous:
		// TODO borrowing a stack address to seed a doubly-linked
		// TODO list.  I understand you can build the new list
		// TODO fragment here unlocked and then only lock to tie
		// TODO the new list to the existing shared list in `state`,
		// TODO but it makes me want to pause and ask if it is
		// TODO necessary.
		//
		// TODO For example, if we built a single-linked list here
		// TODO (with a NULL terminator node) and kept track of the
		// TODO first and last nodes, can we in `fsmonitor_queue_path()`
		// TODO simply connect the parts under lock?
		//
		// TODO That is, do we actually use both directions of the
		// TODO list other than when stitching them together?

		struct fsmonitor_queue_item dummy, *queue = &dummy;
		uint64_t time = getnanotime();

		/* Ensure strictly increasing timestamps */
		pthread_mutex_lock(&state->queue_update_lock);
		if (time <= state->latest_update)
			time = state->latest_update + 1;
		pthread_mutex_unlock(&state->queue_update_lock);

		switch (read_directory_changes_overlapped(state,
							  buffer, sizeof(buffer),
							  &count)) {
		case 0: /* we have valid data */
			break;

		case FSMONITOR_DAEMON_QUIT: /* shutdown event received */
			goto shutdown_event;

		default:
		case -1: /* IO error reading directory events */
			goto force_error_stop;
		}

		/*
		 * If the kernel overflows the buffer that we gave it
		 * (between our calls to ReadDirectoryChangesW()), we
		 * get back a buffer count of zero.  (The call is
		 * successful, but just returns zero bytes.)  This
		 * means that we have lost event notifications.
		 *
		 * Reset our session so that we don't lie to our
		 * clients.
		 */
		if (!count) {
			// TODO Maybe make this verbose only.
			trace2_data_string("fsmonitor", NULL, "rdcw", "overflow");

			// TODO dump cached events and reset token session id.
			// TODO this also means dumping any cookies.
			//
			// TODO goto somewhere to avoid the loops below....
		}

		p = buffer;
		for (;;) {
			FILE_NOTIFY_INFORMATION *info = (void *)p;

			strbuf_reset(&path);
			normalize_path(info, &path);

			switch (fsmonitor_classify_path(path.buf, path.len)) {
			case IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX:
				/* special case cookie files within .git/ */

				// TODO consider only signaling the cookie for the
				// TODO delete event.  because deletes are more
				// TODO deterministic than just a simple close()
				// TODO event. IIRC.

				string_list_append(&state->cookie_list, path.buf + 5);
				break;

			case IS_INSIDE_DOT_GIT:
				/* ignore all other paths inside of .git/ */
				break;

			case IS_DOT_GIT:
				/* .git directory deleted (or renamed away) */
				if ((info->Action == FILE_ACTION_REMOVED) ||
				    (info->Action == FILE_ACTION_RENAMED_OLD_NAME)) {
					trace2_data_string(
						"fsmonitor", NULL, "message",
						".git directory was removed; quitting");
					goto force_shutdown;
				}
				break;

			case IS_WORKTREE_PATH:
			default:
				/* try to queue normal pathname */
				if (fsmonitor_queue_path(&queue, path.buf,
							 path.len, time) < 0) {
					error("could not queue '%s'; exiting",
					      path.buf);
					goto force_error_stop;
				}
				break;
			}

			if (!info->NextEntryOffset)
				break;
			p += info->NextEntryOffset;
		}

		/* Only update the queue if it changed */
		if (queue != &dummy) {
			pthread_mutex_lock(&state->queue_update_lock);
			if (state->first)
				state->first->previous = dummy.previous;
			dummy.previous->next = state->first;
			state->first = queue;
			state->latest_update = time;
			pthread_mutex_unlock(&state->queue_update_lock);
		}

		// TODO (more of a clarification actually)
		// TODO The cookie_list is a list of the cookied observed
		// TODO during the current FS notification event.  These
		// TODO are used to wake up the corresponding `handle_client()`
		// TODO threads.  Then we flush the list in preparation for
		// TODO the next FS notification event.
		// TODO
		// TODO So the cookie_list is the currently observed set and
		// TODO the hashmap is the eventually expected set.

		for (i = 0; i < state->cookie_list.nr; i++)
			fsmonitor_cookie_seen_trigger(state, state->cookie_list.items[i].string);

		string_list_clear(&state->cookie_list, 0);
	}

force_error_stop:
	state->error_code = -1;

force_shutdown:
	/*
	 * Tell the IPC thead pool to stop (which completes the await
	 * in the main thread (which will also signal this thread (if
	 * we are still alive))).
	 */
	ipc_server_stop_async(state->ipc_server_data);

shutdown_event:
	strbuf_release(&path);
	return;
}
