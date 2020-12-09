#include "cache.h"
#include "config.h"
#include "fsmonitor.h"
#include "fsmonitor--daemon.h"

struct fsmonitor_daemon_backend_data
{
	HANDLE hDirWorktree;
	HANDLE hDirGitDir;

	HANDLE hListener[3];
#define LISTENER_SHUTDOWN 0
#define LISTENER_HAVE_DATA_WORKTREE 1
#define LISTENER_HAVE_DATA_GITDIR 2

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
	//
	char buffer_worktree[65536 * sizeof(wchar_t)];
	char buffer_gitdir[65536 * sizeof(wchar_t)];
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
	DWORD *count)
{
	OVERLAPPED overlapped_worktree;
	OVERLAPPED overlapped_gitdir;
	DWORD dwWait;
	BOOL bWorktreeQueued = FALSE;
	BOOL bGitDirQueued = FALSE;
	int result = -1;

	memset(&overlapped_worktree, 0, sizeof(overlapped_worktree));
	ResetEvent(state->backend_data->hListener[LISTENER_HAVE_DATA_WORKTREE]);
	overlapped_worktree.hEvent = state->backend_data->hListener[LISTENER_HAVE_DATA_WORKTREE];

	if (!ReadDirectoryChangesW(state->backend_data->hDirWorktree,
				   state->backend_data->buffer_worktree,
				   sizeof(state->backend_data->buffer_worktree),
				   TRUE,
				   FILE_NOTIFY_CHANGE_FILE_NAME |
				   FILE_NOTIFY_CHANGE_DIR_NAME |
				   FILE_NOTIFY_CHANGE_ATTRIBUTES |
				   FILE_NOTIFY_CHANGE_SIZE |
				   FILE_NOTIFY_CHANGE_LAST_WRITE |
				   FILE_NOTIFY_CHANGE_CREATION,
				   count, &overlapped_worktree, NULL)) {
		error("ReadDirectoryChangedW failed on '%s' [GLE %ld]",
		      state->path_worktree_watch.buf, GetLastError());
		goto cleanup;
	}
	bWorktreeQueued = TRUE;

	if (state->nr_paths_watching > 1) {
		memset(&overlapped_gitdir, 0, sizeof(overlapped_gitdir));
		ResetEvent(state->backend_data->hListener[LISTENER_HAVE_DATA_GITDIR]);
		overlapped_worktree.hEvent = state->backend_data->hListener[LISTENER_HAVE_DATA_WORKTREE];

		if (!ReadDirectoryChangesW(state->backend_data->hDirGitDir,
					   state->backend_data->buffer_gitdir,
					   sizeof(state->backend_data->buffer_gitdir),
					   TRUE,
					   FILE_NOTIFY_CHANGE_FILE_NAME |
					   FILE_NOTIFY_CHANGE_DIR_NAME |
					   FILE_NOTIFY_CHANGE_ATTRIBUTES |
					   FILE_NOTIFY_CHANGE_SIZE |
					   FILE_NOTIFY_CHANGE_LAST_WRITE |
					   FILE_NOTIFY_CHANGE_CREATION,
					   count, &overlapped_gitdir, NULL)) {
			error("ReadDirectoryChangedW failed on '%s' [GLE %ld]",
			      state->path_gitdir_watch.buf, GetLastError());
			goto cleanup;
		}
		bGitDirQueued = TRUE;
	}

	dwWait = WaitForMultipleObjects(1 + /* _SHUTDOWN */
					state->nr_paths_watching,
					state->backend_data->hListener,
					FALSE, INFINITE);

	if (dwWait == WAIT_OBJECT_0 + LISTENER_HAVE_DATA_WORKTREE) {
		if (GetOverlappedResult(state->backend_data->hDirWorktree,
					&overlapped_worktree, count, TRUE))
			result = LISTENER_HAVE_DATA_WORKTREE;
		goto cleanup;
	}

	if (dwWait == WAIT_OBJECT_0 + LISTENER_HAVE_DATA_GITDIR) {
		if (GetOverlappedResult(state->backend_data->hDirGitDir,
					&overlapped_gitdir, count, TRUE))
			result = LISTENER_HAVE_DATA_GITDIR;
		goto cleanup;
	}

	if (dwWait == WAIT_OBJECT_0 + LISTENER_SHUTDOWN) {
		result = LISTENER_SHUTDOWN;
		goto cleanup;
	}

	error("could not read directory changes [GLE %ld]", GetLastError());
	return -1;

cleanup:
	if (bWorktreeQueued && result != LISTENER_HAVE_DATA_WORKTREE) {
		DWORD unused;
		CancelIoEx(state->backend_data->hDirWorktree,
			   &overlapped_worktree);
		GetOverlappedResult(state->backend_data->hDirWorktree,
				    &overlapped_worktree, &unused, TRUE);
	}

	if (bGitDirQueued && result != LISTENER_HAVE_DATA_GITDIR) {
		DWORD unused;
		CancelIoEx(state->backend_data->hDirGitDir,
			   &overlapped_gitdir);
		GetOverlappedResult(state->backend_data->hDirGitDir,
				    &overlapped_gitdir, &unused, TRUE);
	}

	return result;
}

int fsmonitor_listen__ctor(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;
	DWORD desired_access = FILE_LIST_DIRECTORY;
	DWORD share_mode =
		FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE;
	HANDLE dir_w = INVALID_HANDLE_VALUE;
	HANDLE dir_g = INVALID_HANDLE_VALUE;

	dir_w = CreateFileA(state->path_worktree_watch.buf,
			    desired_access, share_mode, NULL, OPEN_EXISTING,
			    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			    NULL);
	if (dir_w == INVALID_HANDLE_VALUE) {
		error(_("[GLE %ld] could not watch '%s'"),
		      GetLastError(), state->path_worktree_watch.buf);

		return -1;
	}

	if (state->nr_paths_watching > 1) {
		dir_g = CreateFileA(state->path_gitdir_watch.buf,
				    desired_access, share_mode, NULL, OPEN_EXISTING,
				    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
				    NULL);
		if (dir_g == INVALID_HANDLE_VALUE) {
			error(_("[GLE %ld] could not watch '%s'"),
			      GetLastError(), state->path_gitdir_watch.buf);

			CloseHandle(dir_w);
			return -1;
		}
	}

	data = xcalloc(1, sizeof(*data));

	data->hDirWorktree = dir_w;
	data->hDirGitDir = dir_g;

	data->hListener[LISTENER_SHUTDOWN] =
		CreateEvent(NULL, TRUE, FALSE, NULL);
	data->hListener[LISTENER_HAVE_DATA_WORKTREE] =
		CreateEvent(NULL, TRUE, FALSE, NULL);
	if (state->nr_paths_watching > 1)
		data->hListener[LISTENER_HAVE_DATA_GITDIR] =
			CreateEvent(NULL, TRUE, FALSE, NULL);
	else
		data->hListener[LISTENER_HAVE_DATA_GITDIR] =
			INVALID_HANDLE_VALUE;

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
	if (data->hListener[LISTENER_HAVE_DATA_WORKTREE] != INVALID_HANDLE_VALUE)
		CloseHandle(data->hListener[LISTENER_HAVE_DATA_WORKTREE]);
	if (data->hListener[LISTENER_HAVE_DATA_GITDIR] != INVALID_HANDLE_VALUE)
		CloseHandle(data->hListener[LISTENER_HAVE_DATA_GITDIR]);

	if (data->hDirWorktree != INVALID_HANDLE_VALUE)
		CloseHandle(data->hDirWorktree);
	if (data->hDirGitDir != INVALID_HANDLE_VALUE)
		CloseHandle(data->hDirGitDir);

	FREE_AND_NULL(state->backend_data);
}

// TODO RDCW() has a note in [1] WRT short-names.  We should understand
// TODO what it means.
// TODO [1] https://docs.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw

void fsmonitor_listen__loop(struct fsmonitor_daemon_state *state)
{

	struct strbuf path = STRBUF_INIT;
	struct string_list cookie_list = STRING_LIST_INIT_DUP;

top:
	for (;;) {
		struct fsmonitor_batch *batch = NULL;
		DWORD count = 0;
		const char *p;
		int r;

		r = read_directory_changes_overlapped(state, &count);
		switch (r) {
		case LISTENER_HAVE_DATA_WORKTREE:
			p = state->backend_data->buffer_worktree;
			break;

		case LISTENER_HAVE_DATA_GITDIR:
			p = state->backend_data->buffer_gitdir;
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

		for (;;) {
			FILE_NOTIFY_INFORMATION *info = (void *)p;
			const char *slash;
			enum fsmonitor_path_type t;

			/*
			 * On Windows, `info` contains an "array" of paths
			 * that are relative to the root of whichever
			 * directory handle received the event.
			 */
			strbuf_reset(&path);
			if (normalize_path_in_utf8(info, &path) == -1)
				goto skip_this_path;

			if (state->nr_paths_watching == 1)
				t = fsmonitor_classify_path_worktree_relative(
					state, path.buf);
			else if (r == LISTENER_HAVE_DATA_WORKTREE)
				t = fsmonitor_classify_path_worktree_relative(
					state, path.buf);
			else
				t = fsmonitor_classify_path_gitdir_relative(
					state, path.buf);

			switch (t) {

			case IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX:
				/* special case cookie files within gitdir */

				/*
				 * Use <gitdir> relative path for cookie file.
				 */
				slash = find_last_dir_sep(path.buf);
				string_list_append(&cookie_list,
						   slash ? slash + 1 : path.buf);
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
