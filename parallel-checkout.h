
#ifndef PARALLEL_CHECKOUT_H
#define PARALLEL_CHECKOUT_H

struct cache_entry;
struct checkout;
struct conv_attrs;
struct progress;
struct unpack_trees_options;

enum parallel_checkout_mode {
	/*
	 * parallel-checkout is disabled.
	 */
	PCM__NONE = 0,

	/*
	 * The checkout--helper processes are throttled and must wait
	 * for a sync request to write an item to the worktree.
	 *
	 * Use this mode when switching branches where we have to spend
	 * most of the time confirming that we won't overwrite uncomitted
	 * changes to existing files in the work tree.
	 */
	PCM__SYNCHRONOUS,

	/*
	 * The checkout--helper process is not throttled and can write
	 * items to the worktree as soon as the blobs are available in
	 * memory.
	 *
	 * For example, we can use this mode when doing a clone when we
	 * do not expect existing files in the work tree and minimal
	 * collisions with untracked/dirty files.
	 */
	PCM__ASYNCHRONOUS,
};

int is_parallel_checkout_mode(const struct checkout *state,
			      enum parallel_checkout_mode mode);

int is_eligible_for_parallel_checkout(const struct conv_attrs *ca);

enum parallel_checkout_mode setup_parallel_checkout(
	struct checkout *state,
	struct unpack_trees_options *o);

void finish_parallel_checkout(struct checkout *state);

int parallel_checkout__sync__write_entry(const struct checkout *state,
					 struct cache_entry *ce);

int parallel_checkout__async__progress(const struct checkout *state,
				       struct progress *progress,
				       unsigned *result_cnt);

int parallel_checkout__async__classify_result(const struct checkout *state,
					      struct cache_entry *ce,
					      struct progress *progress,
					      unsigned *result_cnt);

int parallel_checkout__created_file(struct cache_entry *ce);

int parallel_checkout_enabled(void);

int parallel_checkout_threshold(void);

#endif /* PARALLEL_CHECKOUT_H */
