#include "cache.h"
#include "config.h"
#include "dir.h"
#include "ewah/ewok.h"
#include "fsmonitor.h"
#include "run-command.h"
#include "strbuf.h"
#include "trace2.h"

#define INDEX_EXTENSION_VERSION1	(1)
#define INDEX_EXTENSION_VERSION2	(2)
#define HOOK_INTERFACE_VERSION1		(1)
#define HOOK_INTERFACE_VERSION2		(2)

struct trace_key trace_fsmonitor = TRACE_KEY_INIT(FSMONITOR);

static void fsmonitor_ewah_callback(size_t pos, void *is)
{
	struct index_state *istate = (struct index_state *)is;
	struct cache_entry *ce;

	if (pos >= istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" >= %u)",
		    (uintmax_t)pos, istate->cache_nr);

	ce = istate->cache[pos];
	ce->ce_flags &= ~CE_FSMONITOR_VALID;
}

static int fsmonitor_hook_version(void)
{
	int hook_version;

	if (git_config_get_int("core.fsmonitorhookversion", &hook_version))
		return -1;

	if (hook_version == HOOK_INTERFACE_VERSION1 ||
	    hook_version == HOOK_INTERFACE_VERSION2)
		return hook_version;

	warning("Invalid hook version '%i' in core.fsmonitorhookversion. "
		"Must be 1 or 2.", hook_version);
	return -1;
}

int read_fsmonitor_extension(struct index_state *istate, const void *data,
	unsigned long sz)
{
	const char *index = data;
	uint32_t hdr_version;
	uint32_t ewah_size;
	struct ewah_bitmap *fsmonitor_dirty;
	int ret;
	uint64_t timestamp;
	struct strbuf last_update = STRBUF_INIT;

	if (sz < sizeof(uint32_t) + 1 + sizeof(uint32_t))
		return error("corrupt fsmonitor extension (too short)");

	hdr_version = get_be32(index);
	index += sizeof(uint32_t);
	if (hdr_version == INDEX_EXTENSION_VERSION1) {
		timestamp = get_be64(index);
		strbuf_addf(&last_update, "%"PRIu64"", timestamp);
		index += sizeof(uint64_t);
	} else if (hdr_version == INDEX_EXTENSION_VERSION2) {
		strbuf_addstr(&last_update, index);
		index += last_update.len + 1;
	} else {
		return error("bad fsmonitor version %d", hdr_version);
	}

	istate->fsmonitor_last_update = strbuf_detach(&last_update, NULL);

	ewah_size = get_be32(index);
	index += sizeof(uint32_t);

	fsmonitor_dirty = ewah_new();
	ret = ewah_read_mmap(fsmonitor_dirty, index, ewah_size);
	if (ret != ewah_size) {
		ewah_free(fsmonitor_dirty);
		return error("failed to parse ewah bitmap reading fsmonitor index extension");
	}
	istate->fsmonitor_dirty = fsmonitor_dirty;

	if (!istate->split_index &&
	    istate->fsmonitor_dirty->bit_size > istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
		    (uintmax_t)istate->fsmonitor_dirty->bit_size, istate->cache_nr);

	trace_printf_key(&trace_fsmonitor,
			 "read fsmonitor extension successful [v %d][last_update '%s'][bit_size %"PRIuMAX"]",
			 hdr_version,
			 istate->fsmonitor_last_update,
			 (uintmax_t)istate->fsmonitor_dirty->bit_size);
	return 0;
}

void fill_fsmonitor_bitmap(struct index_state *istate)
{
	unsigned int i, skipped = 0;
	istate->fsmonitor_dirty = ewah_new();
	for (i = 0; i < istate->cache_nr; i++) {
		if (istate->cache[i]->ce_flags & CE_REMOVE)
			skipped++;
		else if (!(istate->cache[i]->ce_flags & CE_FSMONITOR_VALID))
			ewah_set(istate->fsmonitor_dirty, i - skipped);
	}
}

void write_fsmonitor_extension(struct strbuf *sb, struct index_state *istate)
{
	uint32_t hdr_version;
	uint32_t ewah_start;
	uint32_t ewah_size = 0;
	int fixup = 0;
	uintmax_t bit_size;

	if (!istate->split_index &&
	    istate->fsmonitor_dirty->bit_size > istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
		    (uintmax_t)istate->fsmonitor_dirty->bit_size, istate->cache_nr);

	bit_size = istate->fsmonitor_dirty->bit_size;

	put_be32(&hdr_version, INDEX_EXTENSION_VERSION2);
	strbuf_add(sb, &hdr_version, sizeof(uint32_t));

	strbuf_addstr(sb, istate->fsmonitor_last_update);
	strbuf_addch(sb, 0); /* Want to keep a NUL */

	fixup = sb->len;
	strbuf_add(sb, &ewah_size, sizeof(uint32_t)); /* we'll fix this up later */

	ewah_start = sb->len;
	ewah_serialize_strbuf(istate->fsmonitor_dirty, sb);
	ewah_free(istate->fsmonitor_dirty);
	istate->fsmonitor_dirty = NULL;

	/* fix up size field */
	put_be32(&ewah_size, sb->len - ewah_start);
	memcpy(sb->buf + fixup, &ewah_size, sizeof(uint32_t));

	trace_printf_key(&trace_fsmonitor,
			 "write fsmonitor extension successful [v %d][last_update '%s'][bit_size %"PRIuMAX"]",
			 get_be32(&hdr_version), istate->fsmonitor_last_update, bit_size);
}

/*
 * Call the query-fsmonitor hook passing the last update token of the saved results.
 */
static int query_fsmonitor(int version, const char *last_update, struct strbuf *query_result)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	if (!core_fsmonitor)
		return -1;

	if (!strcmp(core_fsmonitor, ":internal:")) {
#ifdef HAVE_FSMONITOR_DAEMON_BACKEND
		return fsmonitor__send_ipc_query(last_update, query_result);
#else
		/* Fake a trivial response. */
		warning(_("fsmonitor--daemon unavailable; falling back"));
		strbuf_add(query_result, "/", 2);
		return 0;
#endif
	}

	strvec_push(&cp.args, core_fsmonitor);
	strvec_pushf(&cp.args, "%d", version);
	strvec_pushf(&cp.args, "%s", last_update);
	cp.use_shell = 1;
	cp.dir = get_git_work_tree();

	return capture_command(&cp, query_result, 1024);
}

static void fsmonitor_refresh_callback(struct index_state *istate, char *name)
{
	int i, len = strlen(name);
	if (name[len - 1] == '/') {
		/* Mark all entries for the folder invalid */
		for (i = 0; i < istate->cache_nr; i++) {
			if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID &&
			    starts_with(istate->cache[i]->name, name))
				istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;
		}
		/* Need to remove the / from the path for the untracked cache */
		name[len - 1] = '\0';
	} else {
		int pos = index_name_pos(istate, name, strlen(name));

		if (pos >= 0) {
			struct cache_entry *ce = istate->cache[pos];
			ce->ce_flags &= ~CE_FSMONITOR_VALID;
		}
	}

	/*
	 * Mark the untracked cache dirty even if it wasn't found in the index
	 * as it could be a new untracked file.
	 */
	trace_printf_key(&trace_fsmonitor, "fsmonitor_refresh_callback '%s'", name);
	untracked_cache_invalidate_path(istate, name, 0);
}

void refresh_fsmonitor(struct index_state *istate)
{
	struct strbuf query_result = STRBUF_INIT;
	int query_success = 0, hook_version = -1;
	size_t bol = 0; /* beginning of line */
	uint64_t last_update;
	struct strbuf last_update_token = STRBUF_INIT;
	char *buf;
	unsigned int i;

	if (!core_fsmonitor || istate->fsmonitor_has_run_once)
		return;

	hook_version = fsmonitor_hook_version();

	istate->fsmonitor_has_run_once = 1;

	trace_printf_key(&trace_fsmonitor, "refresh fsmonitor");
	/*
	 * This could be racy so save the date/time now and query_fsmonitor
	 * should be inclusive to ensure we don't miss potential changes.
	 */
	last_update = getnanotime();
	if (hook_version == HOOK_INTERFACE_VERSION1)
		strbuf_addf(&last_update_token, "%"PRIu64"", last_update);

	/*
	 * If we have a last update token, call query_fsmonitor for the set of
	 * changes since that token, else assume everything is possibly dirty
	 * and check it all.
	 */
	if (istate->fsmonitor_last_update) {
		if (hook_version == -1 || hook_version == HOOK_INTERFACE_VERSION2) {
			query_success = !query_fsmonitor(HOOK_INTERFACE_VERSION2,
				istate->fsmonitor_last_update, &query_result);

			if (query_success) {
				if (hook_version < 0)
					hook_version = HOOK_INTERFACE_VERSION2;

				/*
				 * First entry will be the last update token.
				 *
				 * Need to use a char * variable because static
				 * analysis was suggesting to use strbuf_addbuf
				 * but we don't want to copy the entire strbuf
				 * only the chars up to the first NUL.
				 */
				buf = query_result.buf;
				strbuf_addstr(&last_update_token, buf);
				if (!last_update_token.len) {
					warning("Empty last update token.");
					query_success = 0;
				} else {
					bol = last_update_token.len + 1;
				}
			} else if (hook_version < 0) {
				hook_version = HOOK_INTERFACE_VERSION1;
				if (!last_update_token.len)
					strbuf_addf(&last_update_token, "%"PRIu64"", last_update);
			}
		}

		if (hook_version == HOOK_INTERFACE_VERSION1) {
			query_success = !query_fsmonitor(HOOK_INTERFACE_VERSION1,
				istate->fsmonitor_last_update, &query_result);
		}

		trace_performance_since(last_update, "fsmonitor process '%s'", core_fsmonitor);
		trace_printf_key(&trace_fsmonitor, "fsmonitor process '%s' returned %s",
			core_fsmonitor, query_success ? "success" : "failure");
	}

	/* a fsmonitor process can return '/' to indicate all entries are invalid */
	if (query_success && query_result.buf[bol] != '/') {
		/* Mark all entries returned by the monitor as dirty */
		buf = query_result.buf;
		for (i = bol; i < query_result.len; i++) {
			if (buf[i] != '\0')
				continue;
			fsmonitor_refresh_callback(istate, buf + bol);
			bol = i + 1;
		}
		if (bol < query_result.len)
			fsmonitor_refresh_callback(istate, buf + bol);

		/* Now mark the untracked cache for fsmonitor usage */
		if (istate->untracked)
			istate->untracked->use_fsmonitor = 1;
	} else {

		/* We only want to run the post index changed hook if we've actually changed entries, so keep track
		 * if we actually changed entries or not */
		int is_cache_changed = 0;
		/* Mark all entries invalid */
		for (i = 0; i < istate->cache_nr; i++) {
			if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID) {
				is_cache_changed = 1;
				istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;
			}
		}

		/* If we're going to check every file, ensure we save the results */
		if (is_cache_changed)
			istate->cache_changed |= FSMONITOR_CHANGED;

		if (istate->untracked)
			istate->untracked->use_fsmonitor = 0;
	}
	strbuf_release(&query_result);

	/* Now that we've updated istate, save the last_update_token */
	FREE_AND_NULL(istate->fsmonitor_last_update);
	istate->fsmonitor_last_update = strbuf_detach(&last_update_token, NULL);
}

/*
 * The caller wants to turn on FSMonitor.  And when the caller writes
 * the index to disk, a FSMonitor extension should be included.  This
 * requires that `istate->fsmonitor_last_update` not be NULL.  But we
 * have not actually talked to a FSMonitor process yet, so we don't
 * have an initial value for this field.
 *
 * For a protocol V1 FSMonitor process, this field is a formatted
 * "nanoseconds since epoch" field.  However, for a protocol V2
 * FSMonitor process, this field is an opaque token.
 *
 * Historically, `add_fsmonitor()` has initialized this field to the
 * current time for protocol V1 processes.  There are lots of race
 * conditions here, but that code has shipped...
 *
 * The only true solution is to use a V2 FSMonitor and get a current
 * or default token value (that it understands), but we cannot do that
 * until we have actually talked to an instance of the FSMonitor process
 * (and the protocol requires that we send a token first...).
 *
 * For simplicity, just initialize like we have a V1 process and require
 * that V2 processes adapt.
 */
static void initialize_fsmonitor_last_update(struct index_state *istate)
{
	struct strbuf last_update = STRBUF_INIT;

	strbuf_addf(&last_update, "%"PRIu64"", getnanotime());
	istate->fsmonitor_last_update = strbuf_detach(&last_update, NULL);
}

void add_fsmonitor(struct index_state *istate)
{
	unsigned int i;

	if (!istate->fsmonitor_last_update) {
		trace_printf_key(&trace_fsmonitor, "add fsmonitor");
		istate->cache_changed |= FSMONITOR_CHANGED;
		initialize_fsmonitor_last_update(istate);

		/* reset the fsmonitor state */
		for (i = 0; i < istate->cache_nr; i++)
			istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;

		/* reset the untracked cache */
		if (istate->untracked) {
			add_untracked_cache(istate);
			istate->untracked->use_fsmonitor = 1;
		}

		/* Update the fsmonitor state */
		refresh_fsmonitor(istate);
	}
}

void remove_fsmonitor(struct index_state *istate)
{
	if (istate->fsmonitor_last_update) {
		trace_printf_key(&trace_fsmonitor, "remove fsmonitor");
		istate->cache_changed |= FSMONITOR_CHANGED;
		FREE_AND_NULL(istate->fsmonitor_last_update);
	}
}

void tweak_fsmonitor(struct index_state *istate)
{
	unsigned int i;
	int fsmonitor_enabled = git_config_get_fsmonitor();

	if (istate->fsmonitor_dirty) {
		if (fsmonitor_enabled) {
			/* Mark all entries valid */
			for (i = 0; i < istate->cache_nr; i++) {
				istate->cache[i]->ce_flags |= CE_FSMONITOR_VALID;
			}

			/* Mark all previously saved entries as dirty */
			if (istate->fsmonitor_dirty->bit_size > istate->cache_nr)
				BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
				    (uintmax_t)istate->fsmonitor_dirty->bit_size, istate->cache_nr);
			ewah_each_bit(istate->fsmonitor_dirty, fsmonitor_ewah_callback, istate);

			refresh_fsmonitor(istate);
		}

		ewah_free(istate->fsmonitor_dirty);
		istate->fsmonitor_dirty = NULL;
	}

	switch (fsmonitor_enabled) {
	case -1: /* keep: do nothing */
		break;
	case 0: /* false */
		remove_fsmonitor(istate);
		break;
	case 1: /* true */
		add_fsmonitor(istate);
		break;
	default: /* unknown value: do nothing */
		break;
	}
}

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND

GIT_PATH_FUNC(git_path_fsmonitor_ipc, "fsmonitor")

int fsmonitor__send_ipc_query(const char *since_token,
			      struct strbuf *answer)
{
	int ret = -1;
	int fd = -1;
	int tried_to_spawn = 0;
	enum ipc_active_state state = IPC_STATE__OTHER_ERROR;
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;

	trace2_region_enter("fsm_client", "query", NULL);

	trace2_data_string("fsm_client", NULL, "query/command",
			   since_token);

try_again:
	state = ipc_client_try_connect(git_path_fsmonitor_ipc(), &options, &fd);

	switch (state) {
	case IPC_STATE__LISTENING:
		ret = ipc_client_send_command_to_fd(fd, since_token, answer);
		close(fd);

		trace2_data_intmax("fsm_client", NULL,
				   "query/response-length", answer->len);

		if (trace2_is_enabled()) {
			static char trivial_response[3] = { '\0', '/', '\0' };
			int trivial = !memcmp(trivial_response,
					      &answer->buf[answer->len - 3], 3);
			if (trivial)
				trace2_data_intmax(
					"fsm_client", NULL,
					"query/trivial-response",
					trivial);
		}

		goto done;

	case IPC_STATE__NOT_LISTENING:
		ret = error(_("query_daemon: daemon not available"));
		goto done;

	case IPC_STATE__PATH_NOT_FOUND:
		if (tried_to_spawn)
			goto done;

		tried_to_spawn++;
		if (fsmonitor__spawn_daemon())
			goto done;

		/*
		 * Try again, but this time give the daemon a chance to
		 * actually create the pipe/socket.
		 *
		 * Granted, the daemon just started so it can't possibly have
		 * any FS cached yet, so we'll always get a trivial answer.
		 * BUT the answer should include a new token that can serve
		 * as the basis for subsequent requests.
		 */
		options.wait_if_not_found = 1;
		goto try_again;

	case IPC_STATE__INVALID_PATH:
		ret = error(_("query_daemon: invalid path '%s'"),
			    git_path_fsmonitor_ipc());
		goto done;

	case IPC_STATE__OTHER_ERROR:
	default:
		ret = error(_("query_daemon: unspecified error on '%s'"),
			    git_path_fsmonitor_ipc());
		goto done;
	}

done:
	trace2_region_leave("fsm_client", "query", NULL);

	return ret;
}

int fsmonitor__spawn_daemon(void)
{
#ifndef GIT_WINDOWS_NATIVE
	const char *args[] = { "fsmonitor--daemon", "--start", NULL };

	return run_command_v_opt_tr2(args, RUN_COMMAND_NO_STDIN | RUN_GIT_CMD,
				    "fsmonitor");
#else
	const char *args[] = { "git", "fsmonitor--daemon", "--run", NULL };
	int in = open("/dev/null", O_RDONLY);
	int out = open("/dev/null", O_WRONLY);
	int exec_id;
	pid_t pid;

	/*
	 * Try to start the daemon as a long-running process rather than as
	 * a child (in the Trace2 sense).  Log an exec event for the startup.
	 * We won't have an exec-result event unless fails to start or dies
	 * quickly.
	 */
	exec_id = trace2_exec("git", args);

	pid = mingw_spawnvpe("git", args, NULL, NULL, in, out, out);
	close(in);
	close(out);

	if (pid < 0) {
		trace2_exec_result(exec_id, pid);
		return error(_("could not spawn the fsmonitor daemon"));
	}

	/*
	 * The daemon is (probably) still booting up.  We DO NOT spin/wait
	 * for the pipe/socket to become ready.  We ASSUME that our caller
	 * takes care of that.
	 */
	return 0;
#endif
}

enum ipc_active_state fsmonitor__get_ipc_state(void)
{
	return ipc_get_active_state(git_path_fsmonitor_ipc());
}

#endif
