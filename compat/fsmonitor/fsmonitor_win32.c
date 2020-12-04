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

/*
 * Convert the WCHAR path from the notification into UTF8 and
 * then normalize it.
 */
static int normalize_path_in_utf8(FILE_NOTIFY_INFORMATION *info,
				  struct strbuf *normalized_path)
{
	int reserve;
	int len = 0;

	strbuf_reset(normalized_path);
	if (!info->FileNameLength)
		goto normalize;

	/*
	 * Pre-reserve enough space in the UTF8 buffer for
	 * each Unicode WCHAR character to be mapped into a
	 * sequence of 2 UTF8 characters.  That should let us
	 * avoid ERROR_INSUFFICIENT_BUFFER 99.9+% of the time.
	 */
	reserve = info->FileNameLength + 1;
	strbuf_grow(normalized_path, reserve);

	for (;;) {
		len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
					  info->FileNameLength / sizeof(WCHAR),
					  normalized_path->buf,
					  strbuf_avail(normalized_path) - 1,
					  NULL, NULL);
		if (len > 0)
			goto normalize;
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			error("[GLE %ld] could not convert path to UTF-8: '%.*S'",
			      GetLastError(),
			      (int)(info->FileNameLength / sizeof(WCHAR)),
			      info->FileName);
			return -1;
		}

		strbuf_grow(normalized_path,
			    strbuf_avail(normalized_path) + reserve);
	}

normalize:
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
				   count, &overlapped, NULL)) {
		error("ReadDirectoryChangedW failed [GLE %ld]", GetLastError());
		return -1;
	}

	dwWait = WaitForMultipleObjects(
		ARRAY_SIZE(state->backend_data->hListener),
		state->backend_data->hListener, FALSE, INFINITE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_HAVE_DATA &&
	    GetOverlappedResult(state->backend_data->hDir, &overlapped, count, TRUE))
		return LISTENER_HAVE_DATA;

	CancelIoEx(state->backend_data->hDir, &overlapped);
	GetOverlappedResult(state->backend_data->hDir, &overlapped, count, TRUE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_SHUTDOWN)
		return LISTENER_SHUTDOWN;

	error("could not read directory changes [GLE %ld]", GetLastError());
	return -1;
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
	struct string_list cookie_list = STRING_LIST_INIT_DUP;

top:
	for (;;) {
		struct fsmonitor_batch *batch = NULL;
		int r;

		r = read_directory_changes_overlapped(state,
						      buffer, sizeof(buffer),
						      &count);
		switch (r) {
		case LISTENER_HAVE_DATA:
			break;

		case LISTENER_SHUTDOWN:
			goto shutdown_event;

		default:
		case -1:
			goto force_error_stop;
		}

		/*
		 * If the kernel gets more events than will fit in the kernel
		 * buffer associated with our RDCW handle, it drops them and
		 * returns a count of zero.  (A successful call, but with
		 * length zero.)
		 */
		if (!count) {
			trace2_data_string("fsmonitor", NULL, "fsm-listen/kernel",
					   "overflow");
			fsmonitor_force_resync(state);
			goto top;
		}

		p = buffer;
		for (;;) {
			FILE_NOTIFY_INFORMATION *info = (void *)p;

			strbuf_reset(&path);
			if (normalize_path_in_utf8(info, &path) == -1)
				goto skip_this_path;

			switch (fsmonitor_classify_path(path.buf, path.len)) {
			case IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX:
				/* special case cookie files within .git/ */

				// TODO consider only adding the cookie on the
				// TODO file delete event.  because they are more
				// TODO deterministic than just a simple close()
				// TODO event. IIRC.

				string_list_append(&cookie_list, path.buf + 5);
				break;

			case IS_INSIDE_DOT_GIT:
				/* ignore all other paths inside of .git/ */
				break;

			case IS_DOT_GIT:
				/* .git directory deleted (or renamed away) */
				if ((info->Action == FILE_ACTION_REMOVED) ||
				    (info->Action == FILE_ACTION_RENAMED_OLD_NAME)) {
					trace2_data_string("fsmonitor", NULL,
							   "fsm-listen/dotgit",
							   "removed");

					if (fsmonitor_batch__free(batch))
						BUG("batch should not have a next");
					goto force_shutdown;
				}
				break;

			case IS_WORKTREE_PATH:
			default:
				/* queue normal pathname */
				if (!batch)
					batch = fsmonitor_batch__new();
				fsmonitor_batch__add_path(batch, path.buf);
				break;
			}

skip_this_path:
			if (!info->NextEntryOffset)
				break;
			p += info->NextEntryOffset;
		}

		fsmonitor_publish(state, batch, &cookie_list);
		string_list_clear(&cookie_list, 0);
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
