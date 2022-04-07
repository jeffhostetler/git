#include "test-tool.h"
#include "cache.h"
#include "config.h"
#include "blob.h"
#include "tree.h"
#include "commit.h"
#include "parse-options.h"

static const char * const dump_index_usage[] = {
	"test-tool dump-index [<options>]",
	"",
	"Dumps the contents of the index.",
	NULL
};

static int print_initial_index = 1;
static int print_initial_delta = 0;
static int do_rollback = 0;
static int print_rollback_delta = 0;
static int print_refresh_index = 0;
static int print_refresh_delta = 0;

static struct option options[] = {
	OPT_BOOL(0, "print-initial-index", &print_initial_index, "print initial index as read from disk"),
	OPT_BOOL(0, "print-initial-delta", &print_initial_delta, "print wd delta from initial index"),

	OPT_BOOL(0, "rollback", &do_rollback, "backdate index mtimes before refresh"),
	OPT_BOOL(0, "print-rollback-delta", &print_rollback_delta, "print wd delta after rollback"),

	OPT_BOOL(0, "print-refresh-index", &print_refresh_index, "print refreshed index"),
	OPT_BOOL(0, "print-refresh-delta", &print_refresh_delta, "print wd delta from refreshed index"),
	OPT_END()
};


static void print_lstat_delta_entry(struct cache_entry *ce)
{
	const struct stat_data *sd = &ce->ce_stat_data;
	struct stat st;
	struct stat_data sd_now;

	if (lstat(ce->name, &st) < 0)
		die_errno("could not lstat('%s')", ce->name);
	fill_stat_data(&sd_now, &st);

	if ((sd_now.sd_ctime.sec > sd->sd_ctime.sec) ||
	    (sd_now.sd_ctime.sec == sd->sd_ctime.sec && sd_now.sd_ctime.nsec > sd->sd_ctime.nsec))
		printf(" [d_ctime: +]");
	else if (sd_now.sd_ctime.sec == sd->sd_ctime.sec && sd_now.sd_ctime.nsec == sd->sd_ctime.nsec)
		printf(" [d_ctime: =]");
	else
		printf(" [d_ctime: -]");

	if ((sd_now.sd_mtime.sec > sd->sd_mtime.sec) ||
	    (sd_now.sd_mtime.sec == sd->sd_mtime.sec && sd_now.sd_mtime.nsec > sd->sd_mtime.nsec))
		printf(" [d_mtime: +]");
	else if (sd_now.sd_mtime.sec == sd->sd_mtime.sec && sd_now.sd_mtime.nsec == sd->sd_mtime.nsec)
		printf(" [d_mtime: =]");
	else
		printf(" [d_mtime: -]");

	printf(" [ce_flags: %08x]", ce->ce_flags);

	printf("\t%s\n", ce->name);
}

static void print_lstat_delta(struct index_state *istate)
{
	int k;
	for (k = 0; k < istate->cache_nr; k++)
		print_lstat_delta_entry(istate->cache[k]);
}

static void print_cache_entry(struct cache_entry *ce)
{
	const char *type;
	const struct stat_data *sd = &ce->ce_stat_data;

	printf("%06o ", ce->ce_mode & 0177777);

	if (S_ISSPARSEDIR(ce->ce_mode))
		type = tree_type;
	else if (S_ISGITLINK(ce->ce_mode))
		type = commit_type;
	else
		type = blob_type;

	printf("%s %s", type, oid_to_hex(&ce->oid));
	printf(" [mode2: %8x]", (ce->ce_mode & ~0177777));
	printf(" [ctime: %08x:%08x]", sd->sd_ctime.sec, sd->sd_ctime.nsec);
	printf(" [mtime: %08x:%08x]", sd->sd_mtime.sec, sd->sd_mtime.nsec);
	printf(" [dev: %u ino: %u]", sd->sd_dev, sd->sd_ino);
	printf(" [uid: %u gid: %u]", sd->sd_uid, sd->sd_gid);
	printf(" [flags: %08x]", ce->ce_flags);
	printf(" [size: %10u]", sd->sd_size);
	printf("\t%s\n", ce->name);
}

static void print_cache(struct index_state *istate)
{
	int k;
	for (k = 0; k < istate->cache_nr; k++)
		print_cache_entry(istate->cache[k]);
}

static void rollback_index_entry(struct cache_entry *ce)
{
	struct stat_data *sd = &ce->ce_stat_data;

	sd->sd_mtime.sec -= 5;
}

static void rollback_index(struct index_state *istate)
{
	int k;
	for (k = 0; k < istate->cache_nr; k++)
		rollback_index_entry(istate->cache[k]);
}


int cmd__dump_index(int argc, const char **argv)
{
	struct repository *r = the_repository;

	argc = parse_options(argc, argv, NULL, options, dump_index_usage, 0);
	if (argc)
		usage_with_options(dump_index_usage, options);

	initialize_the_repository();

	setup_git_directory();
	git_config(git_default_config, NULL);

	repo_read_index(r);

	if (print_initial_index) {
		printf("===Initial===\n");
		print_cache(r->index);
	}

	if (print_initial_delta) {
		printf("===Delta===\n");
		print_lstat_delta(r->index);
	}

	if (do_rollback) {
		rollback_index(r->index);
		if (print_rollback_delta) {
			printf("===Delta After Rollback===\n");
			print_lstat_delta(r->index);
		}
	}

	refresh_index(r->index, REFRESH_QUIET, NULL, NULL, NULL);

	if (print_refresh_index) {
		printf("===Refreshed===\n");
		print_cache(r->index);
	}

	if (print_refresh_delta) {
		printf("===Delta After Refresh===\n");
		print_lstat_delta(r->index);
	}

	discard_index(r->index);

	return 0;
}
