#include "cache.h"
#include "config.h"
#include "run-command.h"
#include "version.h"
#include "remote.h"
#include "json-writer.h"
#include "telemetry-plugin.h"

/* no optional events and no options fields in fixed events */
#define TELEMETRY_MASK__NONE                    UINTMAX_C(0)
/* all optional events and all optional fields in all events */
#define TELEMETRY_MASK__ALL                     UINTMAX_MAX

/* these events are optional. */
#define TELEMETRY_MASK__ALIAS_EVENTS            UINTMAX_C(1 <<  1)
#define TELEMETRY_MASK__CHILD_EVENTS            UINTMAX_C(1 <<  2)
#define TELEMETRY_MASK__HOOK_EVENTS             UINTMAX_C(1 <<  3)
#define TELEMETRY_MASK__START_EVENTS            UINTMAX_C(1 <<  4)

/* optional per-event fields */
#define TELEMETRY_MASK__EXIT__BRANCH            UINTMAX_C(1 << 10)
#define TELEMETRY_MASK__EXIT__REPO              UINTMAX_C(1 << 11)

/* want telemetry for sub-commands */
#define TELEMETRY_MASK__SUBCOMMANDS             UINTMAX_C(1 << 20)

static int         my_config_telemetry = -1; /* -1 means unspecified, defaults to off */
static const char *my_config_telemetry_path; /* use stderr if null and no provider */
static int         my_config_telemetry_pretty; /* pretty print telemetry data (debug) */
static uintmax_t   my_config_telemetry_mask = TELEMETRY_MASK__NONE;
static enum telemetry_perf_token my_config_telemetry_perf = TELEMETRY_PERF__NONE;
static struct telemetry_plugin *my_config_telemetry_plugin = NULL;

static int      my_exit_code = -1;
static int      my_is_final_event;
static pid_t    my_pid;
static uint64_t my_ns_start;
static uint64_t my_ns_exit;

static struct strbuf our_sid = STRBUF_INIT;
static struct strbuf parent_sid = STRBUF_INIT;

static struct json_writer jw_alias = JSON_WRITER_INIT;
static struct json_writer jw_argv = JSON_WRITER_INIT;
static struct json_writer jw_errmsg = JSON_WRITER_INIT;
static struct json_writer jw_exit = JSON_WRITER_INIT;
static struct json_writer jw_branch = JSON_WRITER_INIT;
static struct json_writer jw_repo = JSON_WRITER_INIT;

static inline int mask_want(uintmax_t b)
{
	return !!(my_config_telemetry_mask & b);
}

int telemetry_perf_want(enum telemetry_perf_token t)
{
	return !!(my_config_telemetry_perf & t);
}

/*
 * Compute elapsed time in seconds.
 * Inputs are in nanoseconds.
 */
static inline double elapsed(uint64_t ns_end, uint64_t ns_start)
{
	return (double)(ns_end - ns_start)/1000000000;
}

/*
 * If we inherited a parent-SID, we are a sub-command.  It is important
 * that we lookup any SID from the environment before compute_our_sid()
 * overwrites the environment variable.
 */
static int is_subcommand(void)
{
	static int computed = 0;
	const char *parent;

	if (computed)
		return parent_sid.len != 0;
	computed = 1;

	parent = getenv("GIT_TELEMETRY_PARENT_SID");
	if (!parent || !*parent)
		return 0;

	strbuf_addstr(&parent_sid, parent);
	return 1;
}

/*
 * Compute a new SID for the current process.
 */
static void compute_our_sid(void)
{
	/*
	 * A "session id" (SID) is a cheap, unique-enough string to associate
	 * a parent process with its child processes.  This is stronger than
	 * a simple parent-pid because we may have an intermediate shell
	 * between a top-level Git command and a child Git command.
	 *
	 * This could be a UUID/GUID, but that requires extra library
	 * dependencies, is more expensive to compute, and overkill for our
	 * needs.  We just need enough uniqueness to associate a parent and
	 * child process in a telemetry dump (possibly when aggregated over
	 * a long time span or across multiple machines).
	 *
	 * A PID and a nanosecond timestamp should be sufficient for this.
	 *
	 * Consumers should consider this an unordered opaque string in case
	 * we decide to switch to a real UUID in the future.
	 */
	strbuf_addf(&our_sid, "%"PRIuMAX"-%d", (uintmax_t)my_ns_start, my_pid);

	/*
	 * If we DID NOT inherit a parent-SID, export our new SID to the
	 * environment so that any child processes descended from us will see
	 * it.  (We want the parent-SID that they see to be the top-most git
	 * process so that we can see the full cost of the top-level command.)
	 */
	if (!is_subcommand())
		setenv("GIT_TELEMETRY_PARENT_SID", our_sid.buf, 1);
}

/*
 * Builtin emit function to write telemetry events to stderr.
 */
static void emit_stderr(struct json_writer *jw)
{
	fprintf(stderr, "%s\n", jw->json.buf);
}

/*
 * Builtin emit function to write telemetry events to a file.
 *
 * TODO Look at locking the file so that we don't get
 * interleaved events from other processes.
 */
static void emit_to_path(struct json_writer *jw)
{
	int fd = open(my_config_telemetry_path, O_WRONLY | O_APPEND | O_CREAT,
		      0666);
	if (fd == -1) {
		warning("could not open '%s' for telemetry: %s",
			my_config_telemetry_path, strerror(errno));
		return;
	}

	strbuf_addch(&jw->json, '\n');
	if (write_in_full(fd, jw->json.buf, jw->json.len) < 0)
		warning("could not write telemetry event to '%s': %s",
			my_config_telemetry_path, strerror(errno));
	strbuf_setlen(&jw->json, jw->json.len - 1);
	close(fd);
}

static void emit_to_plugin(struct json_writer *jw)
{
	telemetry_plugin_event(my_config_telemetry_plugin, jw->json.buf,
			       my_is_final_event);
}

static void (*my_emit_event)(struct json_writer *jw) = emit_stderr;

static inline int config_telemetry(const char *var, const char *value)
{
	my_config_telemetry = git_config_bool(var, value);
	return 0;
}

static inline int config_telemetrypretty(const char *var, const char *value)
{
	my_config_telemetry_pretty = git_config_bool(var, value);
	return 0;
}

static inline int config_telemetrypath(const char *var, const char *value)
{
	if (!is_absolute_path(value)) {
		warning("telemetry.path must be absolute path: %s",
			value);
		my_config_telemetry = 0;
		return 0;
	}

	my_config_telemetry_path = xstrdup(value);
	if (my_config_telemetry == -1)
		my_config_telemetry = 1;

	return 0;
}

/*
 * Parse the value of mask and set individual bits.  It should be either a
 * boolean value (meaning all or no optional data) or a list of words (with
 * or without delimiters of bits to turn on).
 */
static inline int config_telemetrymask(const char *var, const char *value)
{
	int bool_value = git_parse_maybe_bool(value);

	if (bool_value == 1) {
		my_config_telemetry_mask = TELEMETRY_MASK__ALL;
		return 0;
	}
	if (bool_value == 0) {
		my_config_telemetry_mask = TELEMETRY_MASK__NONE;
		return 0;
	}

	my_config_telemetry_mask = TELEMETRY_MASK__NONE;

	if (strstr(value, "alias"))
		my_config_telemetry_mask |= TELEMETRY_MASK__ALIAS_EVENTS;
	if (strstr(value, "child"))
		my_config_telemetry_mask |= TELEMETRY_MASK__CHILD_EVENTS;
	if (strstr(value, "hook"))
		my_config_telemetry_mask |= TELEMETRY_MASK__HOOK_EVENTS;
	if (strstr(value, "start"))
		my_config_telemetry_mask |= TELEMETRY_MASK__START_EVENTS;

	if (strstr(value, "exit-branch"))
		my_config_telemetry_mask |= TELEMETRY_MASK__EXIT__BRANCH;
	if (strstr(value, "exit-repo"))
		my_config_telemetry_mask |= TELEMETRY_MASK__EXIT__REPO;

	if (strstr(value, "subcommand"))
		my_config_telemetry_mask |= TELEMETRY_MASK__SUBCOMMANDS;

	return 0;
}

/*
 * Parse the value of perf flags and set individual bits.  It should either
 * be a boolean value (meaning all or none) or a list of words (with or
 * without delimiters) of the tokens to turn on.
 */
static inline int config_telemetryperf(const char *var, const char *value)
{
	int bool_value = git_parse_maybe_bool(value);

	if (bool_value == 1) {
		my_config_telemetry_perf = TELEMETRY_PERF__ALL;
		return 0;
	}
	if (bool_value == 0) {
		my_config_telemetry_perf = TELEMETRY_PERF__NONE;
		return 0;
	}

	my_config_telemetry_perf = TELEMETRY_PERF__NONE;

	if (strstr(value, "index"))
		my_config_telemetry_perf |= TELEMETRY_PERF__INDEX;
	if (strstr(value, "status"))
		my_config_telemetry_perf |= TELEMETRY_PERF__STATUS;

	return 0;
}

/*
 * Value should contain the plugin's pathname.
 */
static inline int config_telemetryplugin(const char *key, const char *value)
{
	my_config_telemetry_plugin = telemetry_plugin_load(value);
	if (!my_config_telemetry_plugin) {
		/* plugin layer already printed warning */
		my_config_telemetry = 0;
		return 0;
	}

	if (my_config_telemetry == -1)
		my_config_telemetry = 1;
	if (my_config_telemetry == 1)
		my_config_telemetry = telemetry_plugin_initialize(
			my_config_telemetry_plugin);

	return 0;
}

static const char *token_name(enum telemetry_perf_token token)
{
	switch (token)
	{
	case TELEMETRY_PERF__INDEX:
		return "index";

	case TELEMETRY_PERF__STATUS:
		return "status";

	default:
		return "default";
	}
}

static int config_cb(const char *key, const char *value, void *d)
{
	if (!strcmp(key, "telemetry.enable"))
		return config_telemetry(key, value);
	if (!strcmp(key, "telemetry.path"))
		return config_telemetrypath(key, value);
	if (!strcmp(key, "telemetry.pretty"))
		return config_telemetrypretty(key, value);
	if (!strcmp(key, "telemetry.mask"))
		return config_telemetrymask(key, value);
	if (!strcmp(key, "telemetry.perf"))
		return config_telemetryperf(key, value);
	if (!strcmp(key, "telemetry.plugin"))
		return config_telemetryplugin(key, value);

	return 0;
}

static inline void read_early_telemetry_config(void)
{
	read_early_config(config_cb, NULL);

	if (my_config_telemetry_plugin)
		my_emit_event = emit_to_plugin;
	else if (my_config_telemetry_path)
		my_emit_event = emit_to_path;
}

static inline void format_exit_event(struct json_writer *jw)
{
	/* terminate any in-progress data collections */
	if (jw_errmsg.json.len)
		jw_end(&jw_errmsg);
	if (!jw_is_terminated(&jw_branch))
		jw_end(&jw_branch);

	/* build JSON message to describe our exit state */
	jw_init(jw);
	jw_object_begin(jw, my_config_telemetry_pretty);
	{
		jw_object_string(jw, "event", "exit");
		jw_object_intmax(jw, "time", (intmax_t)my_ns_exit);
		jw_object_intmax(jw, "pid", my_pid);

		jw_object_sub_jw(jw, "argv", &jw_argv);
		if (jw_alias.json.len)
			jw_object_sub_jw(jw, "alias", &jw_alias);

		jw_object_intmax(jw, "exit-code", my_exit_code);
		jw_object_double(jw, "elapsed-time", 6,
				 elapsed(my_ns_exit, my_ns_start));
		if (jw_errmsg.json.len)
			jw_object_sub_jw(jw, "error-message", &jw_errmsg);

		if (jw_branch.json.len)
			jw_object_sub_jw(jw, "branches", &jw_branch);

		if (jw_repo.json.len)
			jw_object_sub_jw(jw, "repo", &jw_repo);

		jw_object_string(jw, "sid", our_sid.buf);
		if (parent_sid.len)
			jw_object_string(jw, "parent-sid", parent_sid.buf);

		jw_object_string(jw, "version", git_version_string);
	}
	jw_end(jw);
}

static void my_atexit(void)
{
	if (my_config_telemetry == 1) {
		format_exit_event(&jw_exit);
		my_is_final_event = 1;
		my_emit_event(&jw_exit);
	}

	strbuf_release(&our_sid);
	strbuf_release(&parent_sid);

	jw_release(&jw_argv);
	jw_release(&jw_errmsg);
	jw_release(&jw_exit);
	jw_release(&jw_branch);
	jw_release(&jw_repo);
}

static inline void format_start_event(struct json_writer *jw)
{
	/* build JSON message to describe our initial state */
	jw_init(jw);
	jw_object_begin(jw, my_config_telemetry_pretty);
	{
		jw_object_string(jw, "event", "start");
		jw_object_intmax(jw, "time", (intmax_t)my_ns_start);
		jw_object_intmax(jw, "pid", my_pid);

		jw_object_sub_jw(jw, "argv", &jw_argv);

		jw_object_string(jw, "sid", our_sid.buf);
		if (parent_sid.len)
			jw_object_string(jw, "parent-sid", parent_sid.buf);

		jw_object_string(jw, "version", git_version_string);
	}
	jw_end(jw);
}

/*
 * Initialize telemetry data.  If telemetry is enabled, emit a "start" event.
 * This event is minimal, but does allow watchers to see when we start and
 * operation (such as checkout) and act accordingly.
 */
void telemetry_start_event(int argc, const char **argv)
{
	struct json_writer jw = JSON_WRITER_INIT;

	my_ns_start = getnanotime();
	my_pid = getpid();

	read_early_telemetry_config();
	if (my_config_telemetry < 1)
		return;

	atexit(my_atexit);

	/*
	 * If this is not a top-level command and the user doesn't want
	 * telemetry for sub-commands, quietly turn off telemetry for
	 * this sub-command.
	 */
	if (!mask_want(TELEMETRY_MASK__SUBCOMMANDS) && is_subcommand()) {
		my_config_telemetry = 0;
		return;
	}

	compute_our_sid();

	jw_array_begin(&jw_argv, my_config_telemetry_pretty);
	jw_array_argc_argv(&jw_argv, argc, argv);
	jw_end(&jw_argv);

	if (!mask_want(TELEMETRY_MASK__START_EVENTS))
		return;

	format_start_event(&jw);
	my_emit_event(&jw);
	jw_release(&jw);
}

/*
 * Collect optional fields for the "exit" event.
 * We should only do this when it is safe to do so.
 */
static inline void collect_optional_exit_fields(void)
{
	/*
	 * If the command failed for any reason, we just give up because we
	 * do not know the state of the process and any data structures.
	 * We have to assume someone called die() and gave up.
	 */
	if (my_exit_code)
		return;

	if (mask_want(TELEMETRY_MASK__EXIT__BRANCH))
		telemetry_set_branch("HEAD");

	if (mask_want(TELEMETRY_MASK__EXIT__REPO))
		telemetry_set_repository();
}

/*
 * Record our exit code (or the return code about to be returned from main())
 * and time so we can later write an "exit" event during atexit().
 */
int telemetry_exit_event(int exit_code)
{
	if (my_config_telemetry < 1)
		return exit_code;

	my_ns_exit = getnanotime();
	my_exit_code = exit_code;

	collect_optional_exit_fields();

	return exit_code;
}

/*
 * Accumulate an unbounded JSON array of error messages in the order received.
 * This will be included in the final "exit" event.
 */
void telemetry_set_errmsg(const char *prefix, const char *fmt, va_list ap)
{
	struct strbuf em = STRBUF_INIT;

	if (my_config_telemetry < 1)
		return;

	if (prefix && *prefix)
		strbuf_addstr(&em, prefix);
	strbuf_vaddf(&em, fmt, ap);

	if (!jw_errmsg.json.len)
		jw_array_begin(&jw_errmsg, my_config_telemetry_pretty);

	jw_array_string(&jw_errmsg, em.buf);
	/* leave the errmsg array unterminated for now */

	strbuf_release(&em);
}

/*
 * Format an event message for any type of child process exit.
 */
static inline void format_child_event(const char *type, struct json_writer *jw,
				      uint64_t ns_start, int pid,
				      const char **argv, int exit_code)
{
	uint64_t ns_end = getnanotime();

	/* build JSON message to describe the child process */
	jw_init(jw);
	jw_object_begin(jw, my_config_telemetry_pretty);
	{
		jw_object_string(jw, "event", type);
		jw_object_intmax(jw, "time", (intmax_t)ns_end);
		jw_object_intmax(jw, "pid", my_pid);

		jw_object_string(jw, "sid", our_sid.buf);
		if (parent_sid.len)
			jw_object_string(jw, "parent-sid", parent_sid.buf);

		jw_object_inline_begin_object(jw, "child");
		{
			jw_object_intmax(jw, "pid", pid);
			jw_object_intmax(jw, "exit-code", exit_code);
			jw_object_double(jw, "elapsed-time", 6,
					 elapsed(ns_end, ns_start));

			jw_object_inline_begin_array(jw, "argv");
			{
				jw_array_argv(jw, argv);
			}
			jw_end(jw);
		}
		jw_end(jw);

		jw_object_string(jw, "version", git_version_string);
	}
	jw_end(jw);
}

/*
 * Record child process exit event for unclassified children.
 */
void telemetry_child_event(uint64_t ns_start, int pid, const char **argv,
			   int exit_code)
{
	struct json_writer jw = JSON_WRITER_INIT;

	if (my_config_telemetry < 1)
		return;

	if (!mask_want(TELEMETRY_MASK__CHILD_EVENTS))
		return;

	format_child_event("child", &jw, ns_start, pid, argv, exit_code);
	my_emit_event(&jw);
	jw_release(&jw);
}

/*
 * Record child process exit for a hook process.
 */
void telemetry_hook_event(uint64_t ns_start, int pid, const char **argv,
			  int exit_code)
{
	struct json_writer jw = JSON_WRITER_INIT;

	if (my_config_telemetry < 1)
		return;

	if (!mask_want(TELEMETRY_MASK__HOOK_EVENTS))
		return;

	format_child_event("hook", &jw, ns_start, pid, argv, exit_code);
	my_emit_event(&jw);
	jw_release(&jw);
}

/*
 * Record child process exit for an alias-expansion process.
 */
void telemetry_alias_event(uint64_t ns_start, int pid, const char **argv,
			  int exit_code)
{
	struct json_writer jw = JSON_WRITER_INIT;

	if (my_config_telemetry < 1)
		return;

	/*
	 * Also capture the alias expansion for later reporting in the exit
	 * event for the current process.  Discard any previous alias expansion
	 * since we only care about the net-net final expansion (when we have
	 * nested aliases).
	 */
	jw_init(&jw_alias);

	jw_array_begin(&jw_alias, 0);
	jw_array_argv(&jw_alias, argv);
	jw_end(&jw_alias);

	if (!mask_want(TELEMETRY_MASK__ALIAS_EVENTS))
		return;

	format_child_event("alias", &jw, ns_start, pid, argv, exit_code);
	my_emit_event(&jw);
	jw_release(&jw);
}

/*
 * Capture information for the current branch, remote, and upstream for
 * later logging.
 *
 * We build an array of branches in case the caller wants to log
 * before and after data around a checkout, for example.
 *
 * Warning: calling branch_get() causes remote.c:read_config() to eventually
 * get called and this has a static "loaded" flag to prevent its internal
 * cache from being loaded twice.  If we call this routine BEFORE the cmd_*()
 * routine is called (say prior to "p->fn()" in git.c), then the remotes and
 * branches get loaded into their cache before the comman runs and alters the
 * behavior of some commands (like "git clone --bare . ./foo") that expect to
 * modify the repo and then do the lookups and build the cache.  This causes,
 * for example, "git clone --bare . ./foo" to invoke "git-upload-pack origin"
 * rather than "git-upload-pack <full-path>/." and fails.
 *
 * Warning: If we call this AFTERWARDS (as we are capturing the exit/return
 * code, we run the risk of getting slightly stale data.  If the cmd_*() did
 * not call branch_get() our request will get fresh data, but if the cmd_*()
 * did something to the disk after calling branch_get() it may not update the
 * cache (since it usually assumes it going to immediately exit and doesn't
 * need to bother), so we may get slightly stale results.
 *
 * So, my point is that we should be careful when we call this in various
 * cmd_*() routines.
 *
 * Note: Some of this information may be sensitive (personally identifiable)
 * so we may want to scrub or conditionally omit it.
 */
void telemetry_set_branch(const char *branch_name)
{
	struct json_writer jw = JSON_WRITER_INIT;
	struct branch *branch = NULL;
	struct remote *remote = NULL;
	const char *upstream_refname = NULL;
	struct object_id oid_branch;
	struct object_id oid_upstream;

	if (my_config_telemetry < 1)
		return;

	branch = branch_get(branch_name);
	if (!branch)
		return;
	if (branch->remote_name)
		remote = remote_get(branch->remote_name);
	upstream_refname = branch_get_upstream(branch, NULL);

	jw_object_begin(&jw, my_config_telemetry_pretty);
	{
		jw_object_inline_begin_object(&jw, "branch");
		{
			jw_object_string(&jw, "name", branch->name);
			jw_object_string(&jw, "refname", branch->refname);

			if (!get_oid_commit(branch->refname, &oid_branch))
				jw_object_string(&jw, "oid",
						 oid_to_hex(&oid_branch));
		}
		jw_end(&jw);

		if (branch->remote_name) {
			jw_object_inline_begin_object(&jw, "remote");
			{
				jw_object_string(&jw, "name",
						 branch->remote_name);
				if (remote && remote->url_nr)
					jw_object_string(&jw, "url",
							 remote->url[0]);
			}
			jw_end(&jw);
		}
		
		if (upstream_refname) {
			jw_object_inline_begin_object(&jw, "upstream");
			{
				jw_object_string(&jw, "refname",
						 upstream_refname);
				if (!get_oid_commit(upstream_refname,
						    &oid_upstream))
					jw_object_string(
						&jw, "oid",
						oid_to_hex(&oid_upstream));
			}
			jw_end(&jw);
		}
	}
	jw_end(&jw);

	if (!jw_branch.json.len)
		jw_array_begin(&jw_branch, my_config_telemetry_pretty);
	jw_array_sub_jw(&jw_branch, &jw);
	/* leave branches array unterminated for now */

	jw_release(&jw);
}

/*
 * Capture information about the repository and the working directory for
 * later logging.
 *
 * Warning: we cannot call this until "the_repository" has been initialized.
 * See environment.c:get_git_dir().
 *
 * Note: Some of this information may be sensitive (personally identifiable)
 * so we may want to scrub or conditionally omit it.
 */
void telemetry_set_repository(void)
{
	if (my_config_telemetry < 1)
		return;

	if (!have_git_dir())
		return;

	jw_object_begin(&jw_repo, my_config_telemetry_pretty);
	{
		jw_object_intmax(&jw_repo, "bare", is_bare_repository());
		jw_object_string(&jw_repo, "git-dir",
				 absolute_path(get_git_dir()));

		if (get_git_work_tree())
			jw_object_string(&jw_repo, "worktree",
					 get_git_work_tree());
	}
	jw_end(&jw_repo);
}

void telemetry_perf_event(uint64_t ns_start, enum telemetry_perf_token token,
			  const char *label, const struct json_writer *jw_data)
{
	struct json_writer jw = JSON_WRITER_INIT;
	uint64_t ns_end;
	const char *tn;

	if (my_config_telemetry < 1)
		return;

	if (!telemetry_perf_want(token))
		return;

	ns_end = getnanotime();
	tn = token_name(token);

	if (!jw_is_terminated(jw_data))
		die("telemetry_perf_event[%s/%s]: unterminated data: %s",
		    tn, label, jw_data->json.buf);

	jw_object_begin(&jw, my_config_telemetry_pretty);
	{
		jw_object_string(&jw, "event", "perf");
		jw_object_string(&jw, "token", tn);
		jw_object_string(&jw, "label", label);
		jw_object_intmax(&jw, "pid", my_pid);

		jw_object_double(&jw, "elapsed-time", 6,
				 elapsed(ns_end, ns_start));

		jw_object_sub_jw(&jw, "data", jw_data);

		jw_object_string(&jw, "sid", our_sid.buf);
		if (parent_sid.len)
			jw_object_string(&jw, "parent-sid", parent_sid.buf);

		jw_object_string(&jw, "version", git_version_string);
	}
	jw_end(&jw);

	my_emit_event(&jw);
	jw_release(&jw);
}
