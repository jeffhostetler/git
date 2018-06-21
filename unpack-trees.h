#ifndef UNPACK_TREES_H
#define UNPACK_TREES_H

#include "tree-walk.h"
#include "argv-array.h"
#ifndef NO_PTHREADS
#include "git-compat-util.h"
#include <pthread.h>
#include "mpmcqueue.h"
#endif

#define MAX_UNPACK_TREES 8

struct unpack_trees_options;
struct exclude_list;

typedef int (*merge_fn_t)(const struct cache_entry * const *src,
		struct unpack_trees_options *options);

enum unpack_trees_error_types {
	ERROR_WOULD_OVERWRITE = 0,
	ERROR_NOT_UPTODATE_FILE,
	ERROR_NOT_UPTODATE_DIR,
	ERROR_WOULD_LOSE_UNTRACKED_OVERWRITTEN,
	ERROR_WOULD_LOSE_UNTRACKED_REMOVED,
	ERROR_BIND_OVERLAP,
	ERROR_SPARSE_NOT_UPTODATE_FILE,
	ERROR_WOULD_LOSE_ORPHANED_OVERWRITTEN,
	ERROR_WOULD_LOSE_ORPHANED_REMOVED,
	ERROR_WOULD_LOSE_SUBMODULE,
	NB_UNPACK_TREES_ERROR_TYPES
};

/*
 * Sets the list of user-friendly error messages to be used by the
 * command "cmd" (either merge or checkout), and show_all_errors to 1.
 */
void setup_unpack_trees_porcelain(struct unpack_trees_options *opts,
				  const char *cmd);

/*
 * Frees resources allocated by setup_unpack_trees_porcelain().
 */
void clear_unpack_trees_porcelain(struct unpack_trees_options *opts);

struct unpack_trees_options {
	unsigned int reset,
		     merge,
		     update,
		     index_only,
		     nontrivial_merge,
		     trivial_merges_only,
		     verbose_update,
		     aggressive,
		     skip_unmerged,
		     initial_checkout,
		     diff_index_cached,
		     debug_unpack,
		     skip_sparse_checkout,
		     gently,
		     exiting_early,
		     show_all_errors,
		     dry_run;
	const char *prefix;
	int cache_bottom;
	struct dir_struct *dir;
	struct pathspec *pathspec;
	merge_fn_t fn;
	const char *msgs[NB_UNPACK_TREES_ERROR_TYPES];
	struct argv_array msgs_to_free;
	/*
	 * Store error messages in an array, each case
	 * corresponding to a error message type
	 */
	struct string_list unpack_rejects[NB_UNPACK_TREES_ERROR_TYPES];

	int head_idx;
	int merge_size;

	struct cache_entry *df_conflict_entry;
	void *unpack_data;

	struct index_state *dst_index;
	struct index_state *src_index;
	struct index_state result;

	struct exclude_list *el; /* for internal use */
#ifndef NO_PTHREADS
	/*
	 * Speed up the tree traversal by adding all discovered tree objects
	 * into a queue and have a pool of worker threads process them in
	 * parallel.  Since there is no upper bound on the size of a tree and
	 * each worker thread will be adding discovered tree objects to the
	 * queue, we need an unbounded multi-producer-multi-consumer queue.
	 */
	struct mpmcq queue;

	int nr_threads;
	pthread_t *pthreads;

	/* need a mutex as we don't have fetch_and_add() */
	int remaining_work;
	pthread_mutex_t work_mutex;

	/* The ODB is not thread safe so we must serialize access to it */
	pthread_mutex_t odb_mutex;

	/* various functions that are not thread safe and must be serialized for now */
	pthread_mutex_t unpack_index_entry_mutex;
	pthread_mutex_t unpack_nondirectories_mutex;

#endif
};

extern int unpack_trees(unsigned n, struct tree_desc *t,
		struct unpack_trees_options *options);

int verify_uptodate(const struct cache_entry *ce,
		    struct unpack_trees_options *o);

int threeway_merge(const struct cache_entry * const *stages,
		   struct unpack_trees_options *o);
int twoway_merge(const struct cache_entry * const *src,
		 struct unpack_trees_options *o);
int bind_merge(const struct cache_entry * const *src,
	       struct unpack_trees_options *o);
int oneway_merge(const struct cache_entry * const *src,
		 struct unpack_trees_options *o);

#endif
