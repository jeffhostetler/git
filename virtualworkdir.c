#include "cache.h"
#include "config.h"
#include "dir.h"
#include "hashmap.h"
#include "run-command.h"
#include "virtualworkdir.h"

#define HOOK_INTERFACE_VERSION	(1)

static struct strbuf virtual_workdir_data = STRBUF_INIT;
static struct hashmap virtual_workdir_hashmap;
static struct hashmap parent_directory_hashmap;

struct virtualworkdir {
	struct hashmap_entry ent; /* must be the first member! */
	const char *pattern;
	int patternlen;
};

static unsigned int(*vwdhash)(const void *buf, size_t len);
static int(*vwdcmp)(const char *a, const char *b, size_t len);

static int vwd_hashmap_cmp(const void *unused_cmp_data,
	const void *a, const void *b, const void *key)
{
	const struct virtualworkdir *vwd1 = a;
	const struct virtualworkdir *vwd2 = b;

	return vwdcmp(vwd1->pattern, vwd2->pattern, vwd1->patternlen);
}

static void get_virtual_workdir_data(struct strbuf *vwd_data)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *p;
	int err;

	strbuf_init(vwd_data, 0);

	p = find_hook("virtual-work-dir");
	if (!p)
		die("unable to find virtual-work-dir hook");

	argv_array_push(&cp.args, p);
	argv_array_pushf(&cp.args, "%d", HOOK_INTERFACE_VERSION);
	cp.use_shell = 1;
	cp.dir = get_git_work_tree();

	err = capture_command(&cp, vwd_data, 1024);
	if (err)
		die("unable to load virtual working directory");
}

static int check_includes_hashmap(struct hashmap *map, const char *pattern, int patternlen)
{
	struct strbuf sb = STRBUF_INIT;
	struct virtualworkdir vwd;
	char *slash;

	/* Check straight mapping */
	strbuf_reset(&sb);
	strbuf_add(&sb, pattern, patternlen);
	vwd.pattern = sb.buf;
	vwd.patternlen = sb.len;
	hashmap_entry_init(&vwd, vwdhash(vwd.pattern, vwd.patternlen));
	if (hashmap_get(map, &vwd, NULL)) {
		strbuf_release(&sb);
		return 1;
	}

	/*
	 * Check to see if it matches a directory or any path
	 * underneath it.  In other words, 'a/b/foo.txt' will match
	 * '/', 'a/', and 'a/b/'.
	 */
	slash = strchr(sb.buf, '/');
	while (slash) {
		vwd.pattern = sb.buf;
		vwd.patternlen = slash - sb.buf + 1;
		hashmap_entry_init(&vwd, vwdhash(vwd.pattern, vwd.patternlen));
		if (hashmap_get(map, &vwd, NULL)) {
			strbuf_release(&sb);
			return 1;
		}
		slash = strchr(slash + 1, '/');
	}

	strbuf_release(&sb);
	return 0;
}

static void includes_hashmap_add(struct hashmap *map, const char *pattern, const int patternlen)
{
	struct virtualworkdir *vwd;

	vwd = xmalloc(sizeof(struct virtualworkdir));
	vwd->pattern = pattern;
	vwd->patternlen = patternlen;
	hashmap_entry_init(vwd, vwdhash(vwd->pattern, vwd->patternlen));
	hashmap_add(map, vwd);
}

static void initialize_includes_hashmap(struct hashmap *map, struct strbuf *vwd_data)
{
	char *buf, *entry;
	size_t len;
	int i;

	/*
	 * Build a hashmap of the virtual working directory data we can use to look
	 * for cache entry matches quickly
	 */
	vwdhash = ignore_case ? memihash : memhash;
	vwdcmp = ignore_case ? strncasecmp : strncmp;
	hashmap_init(map, vwd_hashmap_cmp, NULL, 0);

	entry = buf = vwd_data->buf;
	len = vwd_data->len;
	for (i = 0; i < len; i++) {
		if (buf[i] == '\0') {
			includes_hashmap_add(map, entry, buf + i - entry);
			entry = buf + i + 1;
		}
	}
}

/*
 * Return 1 if the requested item is found in the virtual working directory,
 * 0 for not found and -1 for undecided.
 */
int is_included_in_virtualworkdir(const char *pathname, int pathlen)
{
	if (!core_virtualworkdir)
		return -1;

	if (!virtual_workdir_hashmap.tablesize && virtual_workdir_data.len)
		initialize_includes_hashmap(&virtual_workdir_hashmap, &virtual_workdir_data);
	if (!virtual_workdir_hashmap.tablesize)
		return -1;

	return check_includes_hashmap(&virtual_workdir_hashmap, pathname, pathlen);
}

static void parent_directory_hashmap_add(struct hashmap *map, const char *pattern, const int patternlen)
{
	char *slash;
	struct virtualworkdir *vwd;

	/*
	 * Add any directories leading up to the file as the excludes logic
	 * needs to match directories leading up to the files as well. Detect
	 * and prevent unnecessary duplicate entries which will be common.
	 */
	if (patternlen > 1) {
		slash = strchr(pattern + 1, '/');
		while (slash) {
			vwd = xmalloc(sizeof(struct virtualworkdir));
			vwd->pattern = pattern;
			vwd->patternlen = slash - pattern + 1;
			hashmap_entry_init(vwd, vwdhash(vwd->pattern, vwd->patternlen));
			if (hashmap_get(map, vwd, NULL))
				free(vwd);
			else
				hashmap_add(map, vwd);
			slash = strchr(slash + 1, '/');
		}
	}
}

static void initialize_parent_directory_hashmap(struct hashmap *map, struct strbuf *vwd_data)
{
	char *buf, *entry;
	size_t len;
	int i;

	/*
	 * Build a hashmap of the parent directories contained in the virtual
	 * file system data we can use to look for matches quickly
	 */
	vwdhash = ignore_case ? memihash : memhash;
	vwdcmp = ignore_case ? strncasecmp : strncmp;
	hashmap_init(map, vwd_hashmap_cmp, NULL, 0);

	entry = buf = vwd_data->buf;
	len = vwd_data->len;
	for (i = 0; i < len; i++) {
		if (buf[i] == '\0') {
			parent_directory_hashmap_add(map, entry, buf + i - entry);
			entry = buf + i + 1;
		}
	}
}

static int check_directory_hashmap(struct hashmap *map, const char *pathname, int pathlen)
{
	struct strbuf sb = STRBUF_INIT;
	struct virtualworkdir vwd;

	/* Check for directory */
	strbuf_reset(&sb);
	strbuf_add(&sb, pathname, pathlen);
	strbuf_addch(&sb, '/');
	vwd.pattern = sb.buf;
	vwd.patternlen = sb.len;
	hashmap_entry_init(&vwd, vwdhash(vwd.pattern, vwd.patternlen));
	if (hashmap_get(map, &vwd, NULL)) {
		strbuf_release(&sb);
		return 0;
	}

	strbuf_release(&sb);
	return 1;
}

/*
 * Return 1 for exclude, 0 for include and -1 for undecided.
 */
int is_excluded_from_virtualworkdir(const char *pathname, int pathlen, int dtype)
{
	if (!core_virtualworkdir)
		return -1;

	if (dtype != DT_REG && dtype != DT_DIR && dtype != DT_LNK)
		die(_("is_excluded_from_virtualworkdir passed unhandled dtype"));

	if (dtype == DT_REG || dtype == DT_LNK) {
		int ret = is_included_in_virtualworkdir(pathname, pathlen);
		if (ret > 0)
			return 0;
		if (ret == 0)
			return 1;
		return ret;
	}

	if (dtype == DT_DIR) {
		int ret = is_included_in_virtualworkdir(pathname, pathlen);
		if (ret > 0)
			return 0;

		if (!parent_directory_hashmap.tablesize && virtual_workdir_data.len)
			initialize_parent_directory_hashmap(&parent_directory_hashmap, &virtual_workdir_data);
		if (!parent_directory_hashmap.tablesize)
			return -1;

		return check_directory_hashmap(&parent_directory_hashmap, pathname, pathlen);
	}

	return -1;
}

/*
 * Update the CE_SKIP_WORKTREE bits based on the virtual working directory.
 */
void apply_virtualworkdir(struct index_state *istate)
{
	char *buf, *entry;
	int i;

	if (!git_config_get_virtualworkdir())
		return;

	if (!virtual_workdir_data.len)
		get_virtual_workdir_data(&virtual_workdir_data);

	/* set CE_SKIP_WORKTREE bit on all entries */
	for (i = 0; i < istate->cache_nr; i++)
		istate->cache[i]->ce_flags |= CE_SKIP_WORKTREE;

	/* clear CE_SKIP_WORKTREE bit for everything in the virtual working directory */
	entry = buf = virtual_workdir_data.buf;
	for (i = 0; i < virtual_workdir_data.len; i++) {
		if (buf[i] == '\0') {
			int pos, len;

			len = buf + i - entry;

			/* look for a directory wild card (ie "dir1/") */
			if (buf[i - 1] == '/') {
				if (ignore_case)
					adjust_dirname_case(istate, entry);

				pos = index_name_pos(istate, entry, len);
				if (pos < 0) {
					pos = -pos - 1;
					while (pos < istate->cache_nr && !fspathncmp(istate->cache[pos]->name, entry, len)) {
						istate->cache[pos]->ce_flags &= ~CE_SKIP_WORKTREE;
						pos++;
					}
				}
			} else {
				if (ignore_case) {
					struct cache_entry *ce = index_file_exists(istate, entry, len, ignore_case);
					if (ce)
						ce->ce_flags &= ~CE_SKIP_WORKTREE;
				} else {
					int pos = index_name_pos(istate, entry, len);
					if (pos >= 0)
						istate->cache[pos]->ce_flags &= ~CE_SKIP_WORKTREE;
				}
			}

			entry += len + 1;
		}
	}
}

/*
 * Free the virtual working directory data structures.
 */
void free_virtualworkdir(void) {
	hashmap_free(&virtual_workdir_hashmap, 1);
	hashmap_free(&parent_directory_hashmap, 1);
	strbuf_release(&virtual_workdir_data);
}
