#if defined(__GNUC__)
/*
 * It is possible to #include CoreFoundation/CoreFoundation.h when compiling
 * with clang, but not with GCC as of time of writing.
 *
 * See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=93082 for details.
 */
typedef unsigned int FSEventStreamCreateFlags;
#define kFSEventStreamEventFlagNone               0x00000000
#define kFSEventStreamEventFlagMustScanSubDirs    0x00000001
#define kFSEventStreamEventFlagUserDropped        0x00000002
#define kFSEventStreamEventFlagKernelDropped      0x00000004
#define kFSEventStreamEventFlagEventIdsWrapped    0x00000008
#define kFSEventStreamEventFlagHistoryDone        0x00000010
#define kFSEventStreamEventFlagRootChanged        0x00000020
#define kFSEventStreamEventFlagMount              0x00000040
#define kFSEventStreamEventFlagUnmount            0x00000080
#define kFSEventStreamEventFlagItemCreated        0x00000100
#define kFSEventStreamEventFlagItemRemoved        0x00000200
#define kFSEventStreamEventFlagItemInodeMetaMod   0x00000400
#define kFSEventStreamEventFlagItemRenamed        0x00000800
#define kFSEventStreamEventFlagItemModified       0x00001000
#define kFSEventStreamEventFlagItemFinderInfoMod  0x00002000
#define kFSEventStreamEventFlagItemChangeOwner    0x00004000
#define kFSEventStreamEventFlagItemXattrMod       0x00008000
#define kFSEventStreamEventFlagItemIsFile         0x00010000
#define kFSEventStreamEventFlagItemIsDir          0x00020000
#define kFSEventStreamEventFlagItemIsSymlink      0x00040000
#define kFSEventStreamEventFlagOwnEvent           0x00080000
#define kFSEventStreamEventFlagItemIsHardlink     0x00100000
#define kFSEventStreamEventFlagItemIsLastHardlink 0x00200000
#define kFSEventStreamEventFlagItemCloned         0x00400000

typedef struct __FSEventStream *FSEventStreamRef;
typedef const FSEventStreamRef ConstFSEventStreamRef;

typedef unsigned int CFStringEncoding;
#define kCFStringEncodingUTF8 0x08000100

typedef const struct __CFString *CFStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef const struct __CFRunLoop *CFRunLoopRef;

struct FSEventStreamContext {
    long long version;
    void *cb_data, *retain, *release, *copy_description;
};

typedef struct FSEventStreamContext FSEventStreamContext;
typedef unsigned int FSEventStreamEventFlags;
#define kFSEventStreamCreateFlagNoDefer 0x02
#define kFSEventStreamCreateFlagWatchRoot 0x04
#define kFSEventStreamCreateFlagFileEvents 0x10

typedef unsigned long long FSEventStreamEventId;
#define kFSEventStreamEventIdSinceNow 0xFFFFFFFFFFFFFFFFULL

typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef streamRef,
				      void *context,
				      __SIZE_TYPE__ num_of_events,
				      void *event_paths,
				      const FSEventStreamEventFlags event_flags[],
				      const FSEventStreamEventId event_ids[]);
typedef double CFTimeInterval;
FSEventStreamRef FSEventStreamCreate(void *allocator,
				     FSEventStreamCallback callback,
				     FSEventStreamContext *context,
				     CFArrayRef paths_to_watch,
				     FSEventStreamEventId since_when,
				     CFTimeInterval latency,
				     FSEventStreamCreateFlags flags);
CFStringRef CFStringCreateWithCString(void *allocator, const char *string,
				      CFStringEncoding encoding);
CFArrayRef CFArrayCreate(void *allocator, const void **items, long long count,
			 void *callbacks);
void CFRunLoopRun(void);
void CFRunLoopStop(CFRunLoopRef run_loop);
CFRunLoopRef CFRunLoopGetCurrent(void);
extern CFStringRef kCFRunLoopDefaultMode;
void FSEventStreamScheduleWithRunLoop(FSEventStreamRef stream,
				      CFRunLoopRef run_loop,
				      CFStringRef run_loop_mode);
unsigned char FSEventStreamStart(FSEventStreamRef stream);
void FSEventStreamStop(FSEventStreamRef stream);
void FSEventStreamInvalidate(FSEventStreamRef stream);
void FSEventStreamRelease(FSEventStreamRef stream);
#else
/*
 * Let Apple's headers declare `isalnum()` first, before
 * Git's headers override it via a constant
 */
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#endif

#include "cache.h"
#include "fsmonitor.h"
#include "fsmonitor--daemon.h"

struct fsmonitor_daemon_backend_data
{
	struct strbuf watch_dir;
	FSEventStreamRef stream;
	CFStringRef watch_path;
	CFArrayRef paths_to_watch;

	CFRunLoopRef rl;

	enum shutdown_style {
		SHUTDOWN_EVENT = 0,
		FORCE_SHUTDOWN,
		FORCE_ERROR_STOP,
	} shutdown_style;
};

static void log_flags_set(const char *path, const FSEventStreamEventFlags flag) {
	struct strbuf msg = STRBUF_INIT;
	strbuf_addf(&msg, "%s flags: %u = ", path, flag);

	if (flag & kFSEventStreamEventFlagMustScanSubDirs)
		strbuf_addstr(&msg, "MustScanSubDirs|");
	if (flag & kFSEventStreamEventFlagUserDropped)
		strbuf_addstr(&msg, "UserDropped|");
	if (flag & kFSEventStreamEventFlagKernelDropped)
		strbuf_addstr(&msg, "KernelDropped|");
	if (flag & kFSEventStreamEventFlagEventIdsWrapped)
		strbuf_addstr(&msg, "EventIdsWrapped|");
	if (flag & kFSEventStreamEventFlagHistoryDone)
		strbuf_addstr(&msg, "HistoryDone|");
	if (flag & kFSEventStreamEventFlagRootChanged)
		strbuf_addstr(&msg, "RootChanged|");
	if (flag & kFSEventStreamEventFlagMount)
		strbuf_addstr(&msg, "Mount|");
	if (flag & kFSEventStreamEventFlagUnmount)
		strbuf_addstr(&msg, "Unmount|");
	if (flag & kFSEventStreamEventFlagItemChangeOwner)
		strbuf_addstr(&msg, "ItemChangeOwner|");
	if (flag & kFSEventStreamEventFlagItemCreated)
		strbuf_addstr(&msg, "ItemCreated|");
	if (flag & kFSEventStreamEventFlagItemFinderInfoMod)
		strbuf_addstr(&msg, "ItemFinderInfoMod|");
	if (flag & kFSEventStreamEventFlagItemInodeMetaMod)
		strbuf_addstr(&msg, "ItemInodeMetaMod|");
	if (flag & kFSEventStreamEventFlagItemIsDir)
		strbuf_addstr(&msg, "ItemIsDir|");
	if (flag & kFSEventStreamEventFlagItemIsFile)
		strbuf_addstr(&msg, "ItemIsFile|");
	if (flag & kFSEventStreamEventFlagItemIsHardlink)
		strbuf_addstr(&msg, "ItemIsHardlink|");
	if (flag & kFSEventStreamEventFlagItemIsLastHardlink)
		strbuf_addstr(&msg, "ItemIsLastHardlink|");
	if (flag & kFSEventStreamEventFlagItemIsSymlink)
		strbuf_addstr(&msg, "ItemIsSymlink|");
	if (flag & kFSEventStreamEventFlagItemModified)
		strbuf_addstr(&msg, "ItemModified|");
	if (flag & kFSEventStreamEventFlagItemRemoved)
		strbuf_addstr(&msg, "ItemRemoved|");
	if (flag & kFSEventStreamEventFlagItemRenamed)
		strbuf_addstr(&msg, "ItemRenamed|");
	if (flag & kFSEventStreamEventFlagItemXattrMod)
		strbuf_addstr(&msg, "ItemXattrMod|");
	if (flag & kFSEventStreamEventFlagOwnEvent)
		strbuf_addstr(&msg, "OwnEvent|");
	if (flag & kFSEventStreamEventFlagItemCloned)
		strbuf_addstr(&msg, "ItemCloned|");

	// TODO cleanup trace2 and maybe use trace1
	trace2_data_string("fsmonitor", the_repository, "fsevent", msg.buf);
	strbuf_release(&msg);
}

static int ef_is_root_delete(const FSEventStreamEventFlags ef)
{
	// TODO The original fsmonitor_special_path() version used
	// TODO (ef & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagItemRemoved))
	// TODO but my testing is not showing that ...RootChanged is set
	// TODO when the .git directory is deleted.  So I wonder what
	// TODO circumstances cause that bit to be set.  I'm going to
	// TODO ignore it for now.

	return (ef & kFSEventStreamEventFlagItemIsDir &&
		ef & kFSEventStreamEventFlagItemRemoved);
}

static int ef_is_root_renamed(const FSEventStreamEventFlags ef)
{
	return (ef & kFSEventStreamEventFlagItemIsDir &&
		ef & kFSEventStreamEventFlagItemRenamed);
}

static void fsevent_callback(ConstFSEventStreamRef streamRef,
			     void *ctx,
			     size_t num_of_events,
			     void * event_paths,
			     const FSEventStreamEventFlags event_flags[],
			     const FSEventStreamEventId event_ids[])
{
	struct fsmonitor_daemon_state *state = ctx;
	struct fsmonitor_daemon_backend_data *data = state->backend_data;
	char **paths = (char **)event_paths;
	struct fsmonitor_queue_item *queue_head = NULL;
	struct fsmonitor_queue_item *queue_tail = NULL;
	struct string_list cookie_list = STRING_LIST_INIT_DUP;
	int seq_nr;
	int i;

	seq_nr = fsmonitor_get_next_token_seq_nr(state);

	for (i = 0; i < num_of_events; i++) {
		const char *path = paths[i] + data->watch_dir.len;
		size_t len = strlen(path);

		if (*path == '/') {
			path++;
			len--;
		}

		/*
		 * If event[i] is marked as dropped, we assume that we have
		 * lost sync with the filesystem and should flush our cached
		 * data.  We need to:
		 * [1] Discard the list of queue-items that we were locally
		 *     building.
		 * [2] Abort/wake any threads waiting for a cookie.
		 * [3] Flush the cached data and create a new token.
		 *
		 * We assume that any events that we received in this callback
		 * (after event[i]) may still be valid.
		 */
		if ((event_flags[i] & kFSEventStreamEventFlagKernelDropped) ||
		    (event_flags[i] & kFSEventStreamEventFlagUserDropped)) {
			/*
			 * see also kFSEventStreamEventFlagMustScanSubDirs
			 */
			trace2_data_string("fsmonitor", NULL,
					   "fsm-listen/kernel", "dropped");

			fsmonitor_free_private_paths(queue_head);
			queue_head = NULL;
			queue_tail = NULL;

			string_list_clear(&cookie_list, 0);

			fsmonitor_force_resync(state);
			seq_nr = fsmonitor_get_next_token_seq_nr(state);

			continue;
		}

		switch (fsmonitor_classify_path(path, len)) {
		case IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX:
			/* special case cookie files within .git/ */
			string_list_append(&cookie_list, path + 5);
			break;

		case IS_INSIDE_DOT_GIT:
			/* ignore all other paths inside of .git/ */
			break;

		case IS_DOT_GIT:
			/*
			 * If .git directory is deleted or renamed away,
			 * we have to quit.
			 */
			if (ef_is_root_delete(event_flags[i])) {
				trace2_data_string("fsmonitor", NULL,
						   "fsm-listen/dotgit",
						   "removed");
				goto force_shutdown;
			}
			if (ef_is_root_renamed(event_flags[i])) {
				trace2_data_string("fsmonitor", NULL,
						   "fsm-listen/dotgit",
						   "renamed");
				goto force_shutdown;
			}
			break;

		case IS_WORKTREE_PATH:
		default:
			/* try to queue normal pathnames */

			// TODO ONLY-IF trace2 is enabled, should we call this.
			log_flags_set(path, event_flags[i]);

			/* TODO: fsevent could be marked as both a file and directory */

			if (event_flags[i] & kFSEventStreamEventFlagItemIsFile) {
				queue_head = fsmonitor_private_add_path(
					queue_head, path, seq_nr++);
				if (!queue_tail)
					queue_tail = queue_head;
				break;
			}

			if (event_flags[i] & kFSEventStreamEventFlagItemIsDir) {
				char *p = xstrfmt("%s/", path);
				queue_head = fsmonitor_private_add_path(
					queue_head, p, seq_nr++);
				if (!queue_tail)
					queue_tail = queue_head;
				free(p);
				break;
			}

			break;
		}
	}

	fsmonitor_publish_queue_paths(state, queue_head, queue_tail);
	queue_head = NULL;
	queue_tail = NULL;

	if (cookie_list.nr) {
		fsmonitor_cookie_mark_seen(state, &cookie_list);
		string_list_clear(&cookie_list, 0);
	}

	return;

force_shutdown:
	fsmonitor_free_private_paths(queue_head);
	queue_head = NULL;
	queue_tail = NULL;

	data->shutdown_style = FORCE_SHUTDOWN;
	CFRunLoopStop(data->rl);
	return;
}

int fsmonitor_listen__ctor(struct fsmonitor_daemon_state *state)
{
	FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagNoDefer |
		kFSEventStreamCreateFlagWatchRoot |
		kFSEventStreamCreateFlagFileEvents;
	FSEventStreamContext ctx = {
		0,
		state,
		NULL,
		NULL,
		NULL
	};
	struct fsmonitor_daemon_backend_data *data;

	data = xcalloc(1, sizeof(*data));
	state->backend_data = data;

	strbuf_init(&data->watch_dir, 0);
	strbuf_addstr(&data->watch_dir, get_git_work_tree());

	data->watch_path = CFStringCreateWithCString(NULL, data->watch_dir.buf, kCFStringEncodingUTF8);
	data->paths_to_watch = CFArrayCreate(NULL, (const void **)&data->watch_path, 1, NULL);
	data->stream = FSEventStreamCreate(NULL, fsevent_callback, &ctx, data->paths_to_watch,
				     kFSEventStreamEventIdSinceNow, 0.1, flags);
	if (data->stream == NULL)
		goto failed;

	/*
	 * `data->rl` needs to be set inside the listener thread.
	 */

	return 0;

failed:
	error("Unable to create FSEventStream.");
	strbuf_release(&data->watch_dir);
	// TODO destroy data->{watch_path,paths_to_watch} ??
	FREE_AND_NULL(state->backend_data);
	return -1;
}

void fsmonitor_listen__dtor(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;

	if (!state || !state->backend_data)
		return;

	data = state->backend_data;

	strbuf_release(&data->watch_dir);
	// TODO destroy data->{watch_path,paths_to_watch} ??

	if (data->stream) {
		FSEventStreamStop(data->stream);
		FSEventStreamInvalidate(data->stream);
		FSEventStreamRelease(data->stream);
	}

	FREE_AND_NULL(state->backend_data);
}

void fsmonitor_listen__stop_async(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;

	data = state->backend_data;
	data->shutdown_style = SHUTDOWN_EVENT;

	CFRunLoopStop(data->rl);
}

void fsmonitor_listen__request_flush(struct fsmonitor_daemon_state *state)
{
	// TODO implement this.
	// TODO
	// TODO need to inject state flag (under lock) and ...
}

void fsmonitor_listen__loop(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;

	data = state->backend_data;

	// TODO cleanup trace2 and maybe use trace1
	trace2_printf("Start watching: '%s' for fsevents", data->watch_dir.buf);

	data->rl = CFRunLoopGetCurrent();

	FSEventStreamScheduleWithRunLoop(data->stream, data->rl, kCFRunLoopDefaultMode);
	if (!FSEventStreamStart(data->stream)) {
		error("Failed to start the FSEventStream");
		goto force_error_stop_without_loop;
	}

	CFRunLoopRun();

	switch (data->shutdown_style) {
	case FORCE_ERROR_STOP:
		state->error_code = -1;
		/* fall thru */
	case FORCE_SHUTDOWN:
		ipc_server_stop_async(state->ipc_server_data);
		/* fall thru */
	case SHUTDOWN_EVENT:
	default:
		break;
	}
	return;

force_error_stop_without_loop:
	state->error_code = -1;
	ipc_server_stop_async(state->ipc_server_data);
	return;
}
