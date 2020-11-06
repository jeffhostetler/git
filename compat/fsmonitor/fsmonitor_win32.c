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
};

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

	FREE_AND_NULL(state->backend_data);
}

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
	uint64_t seq_nr;

//	pthread_mutex_lock(&state->queue_update_lock);
//	state->current_token_data = fsmonitor_new_token_data();
	seq_nr = 1;
//	pthread_mutex_unlock(&state->queue_update_lock);

	for (;;) {
		struct fsmonitor_queue_item *queue_head = NULL;
		struct fsmonitor_queue_item *queue_tail = NULL;

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
				/* queue normal pathname */
				queue_head = fsmonitor_private_add_path(queue_head,
									path.buf,
									seq_nr++);
				if (!queue_tail)
					queue_tail = queue_head;
				break;
			}

			if (!info->NextEntryOffset)
				break;
			p += info->NextEntryOffset;
		}

		fsmonitor_publish_queue_paths(state, queue_head, queue_tail);
		queue_head = NULL;
		queue_tail = NULL;

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
