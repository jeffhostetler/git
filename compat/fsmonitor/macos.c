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

	trace2_data_string("fsmonitor", the_repository, "fsevent", msg.buf);
	strbuf_release(&msg);
}

static void fsevent_callback(ConstFSEventStreamRef streamRef,
			     void *ctx,
			     size_t num_of_events,
			     void * event_paths,
			     const FSEventStreamEventFlags event_flags[],
			     const FSEventStreamEventId event_ids[])
{
	int i;
	struct stat st;
	char **paths = (char **)event_paths;
	struct fsmonitor_queue_item dummy, *queue = &dummy;
	uint64_t time = getnanotime();
	struct fsmonitor_daemon_state *state = ctx;
	struct fsmonitor_daemon_backend_data *data = state->backend_data;

	/* Ensure strictly increasing timestamps */
	pthread_mutex_lock(&state->queue_update_lock);
	if (time <= state->latest_update)
		time = state->latest_update + 1;
	pthread_mutex_unlock(&state->queue_update_lock);

	for (i = 0; i < num_of_events; i++) {
		int special;
		const char *path = paths[i] + data->watch_dir.len;
		size_t len = strlen(path);

		if (*path == '/') {
			path++;
			len--;
		}

		special = fsmonitor_special_path(
			state, path, len,
			(event_flags[i] & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagItemRemoved)) &&
			lstat(paths[i], &st));

		if ((event_flags[i] & kFSEventStreamEventFlagKernelDropped) ||
		    (event_flags[i] & kFSEventStreamEventFlagUserDropped)) {
			trace2_data_string("fsmonitor", the_repository, "message", "Dropped event");
			fsmonitor_queue_path(state, &queue, "/", 1, time);
		}

		if (special > 0) {
			/* ignore paths inside of .git/ */
		}
		else if (!special) {
			/* try to queue normal pathname */
			log_flags_set(path, event_flags[i]);

			/* TODO: fsevent could be marked as both a file and directory */
			if ((event_flags[i] & kFSEventStreamEventFlagItemIsFile) &&
			    fsmonitor_queue_path(state, &queue, path, len, time) < 0) {
				error("could not queue '%s'; exiting", path);
				data->shutdown_style = FORCE_ERROR_STOP;
				CFRunLoopStop(data->rl);
				return;
			} else if (event_flags[i] & kFSEventStreamEventFlagItemIsDir) {
				char *p = xstrfmt("%s/", path);
				if (fsmonitor_queue_path(state, &queue,
							 p, len + 1,
							 time) < 0) {
					error("could not queue '%s'; exiting", p);
					data->shutdown_style = FORCE_ERROR_STOP;
					free(p);
					CFRunLoopStop(data->rl);
					return;
				}
				free(p);
			}
		} else if (special == FSMONITOR_DAEMON_QUIT) {
			/* .git directory deleted (or renamed away) */
			trace2_data_string("fsmonitor", the_repository,
					   "message", ".git directory being removed so quitting.");
			data->shutdown_style = FORCE_SHUTDOWN;
			CFRunLoopStop(data->rl);
			return;
		} else if (special < 0) {
			BUG("special %d < 0 for '%s'", special, path);
		}
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

	for (i = 0; i < state->cookie_list.nr; i++) {
		fsmonitor_cookie_seen_trigger(state, state->cookie_list.items[i].string);
	}

	string_list_clear(&state->cookie_list, 0);
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

void fsmonitor_listen__loop(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;

	data = state->backend_data;

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
