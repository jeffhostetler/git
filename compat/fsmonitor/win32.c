#include "cache.h"
#include "fsmonitor.h"

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

/*
 * Gently tell the fsmonitor listener thread to stop.
 */
int fsmonitor_listen_stop(struct fsmonitor_daemon_state *state)
{
	SetEvent(state->hListener[LISTENER_SHUTDOWN]);

	return 0;
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
	HANDLE dir,
	char *buffer,
	size_t buffer_len,
	DWORD *count)
{
	OVERLAPPED overlapped;
	DWORD dwWait;

	memset(&overlapped, 0, sizeof(overlapped));

	ResetEvent(state->hListener[LISTENER_HAVE_DATA]);
	overlapped.hEvent = state->hListener[LISTENER_HAVE_DATA];

	if (!ReadDirectoryChangesW(dir, buffer, buffer_len, TRUE,
				   FILE_NOTIFY_CHANGE_FILE_NAME |
				   FILE_NOTIFY_CHANGE_DIR_NAME |
				   FILE_NOTIFY_CHANGE_ATTRIBUTES |
				   FILE_NOTIFY_CHANGE_SIZE |
				   FILE_NOTIFY_CHANGE_LAST_WRITE |
				   FILE_NOTIFY_CHANGE_CREATION,
				   count, &overlapped, NULL))
		return error("ReadDirectoryChangedW failed [GLE %ld]",
			     GetLastError());

	dwWait = WaitForMultipleObjects(2, state->hListener, FALSE, INFINITE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_HAVE_DATA &&
	    GetOverlappedResult(dir, &overlapped, count, TRUE))
		return 0;

	CancelIoEx(dir, &overlapped);
	GetOverlappedResult(dir, &overlapped, count, TRUE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_SHUTDOWN)
		return FSMONITOR_DAEMON_QUIT;

	return error("could not read directory changes");
}



// TODO The `return state` statements assume that someone is going to join
// TODO in order to recover that info, but the caller probably already has
// TODO the state (because it needs the pthread_id to call join).  So this
// TODO is probably not needed.  That is, just return NULL.
//
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

struct fsmonitor_daemon_state *fsmonitor_listen(struct fsmonitor_daemon_state *state)
{
	HANDLE dir;

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

	char buffer[65536 * sizeof(wchar_t)], *p;
	DWORD desired_access = FILE_LIST_DIRECTORY;
	DWORD share_mode =
		FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE;
	DWORD count = 0;
	int i;

	dir = CreateFileW(L".", desired_access, share_mode, NULL, OPEN_EXISTING,
			  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			  NULL);

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
		struct strbuf path = STRBUF_INIT;
		uint64_t time = getnanotime();

		/* Ensure strictly increasing timestamps */
		pthread_mutex_lock(&state->queue_update_lock);
		if (time <= state->latest_update)
			time = state->latest_update + 1;
		pthread_mutex_unlock(&state->queue_update_lock);

		switch (read_directory_changes_overlapped(state, dir,
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

		p = buffer;
		for (;;) {
			FILE_NOTIFY_INFORMATION *info = (void *)p;
			int special;

			normalize_path(info, &path);

			special = fsmonitor_special_path(state, path.buf,
							 path.len,
							 info->Action ==
							 FILE_ACTION_REMOVED);

			if (special > 0) {
				/* ignore paths inside of .git/ */
			}
			else if (!special) {
				/* try to queue normal pathname */
				if (fsmonitor_queue_path(state, &queue, path.buf,
							 path.len, time) < 0) {
					error("could not queue '%s'; exiting",
					      path.buf);
					goto force_error_stop;
				}
			} else if (special == FSMONITOR_DAEMON_QUIT) {
				/* .git directory deleted */
				trace2_printf("observed SHUTDOWN from .git dir");
				goto force_shutdown;
			} else {
				BUG("special %d < 0 for '%s'", special, path.buf);
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
		strbuf_release(&path);
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
	CloseHandle(dir);
	return state;
}
