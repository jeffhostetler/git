#include "test-tool.h"
#include "cache.h"
#include "config.h"
#include "blob.h"
#include "tree.h"
#include "commit.h"

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

int cmd__dump_index(int argc, const char **argv)
{
	struct repository *r = the_repository;

	initialize_the_repository();

	setup_git_directory();
	git_config(git_default_config, NULL);

	repo_read_index(r);
	printf("===Initial===\n");
	print_cache(r->index);

	refresh_index(r->index, REFRESH_QUIET, NULL, NULL, NULL);
	printf("===Refreshed===\n");
	print_cache(r->index);

	discard_index(r->index);

	return 0;
}
