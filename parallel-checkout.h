
#ifndef PARALLEL_CHECKOUT_H
#define PARALLEL_CHECKOUT_H

struct unpack_trees_options;

int is_eligible_for_parallel_checkout(const struct conv_attrs *ca);

void setup_parallel_checkout(struct checkout *state,
			     struct unpack_trees_options *o);
void finish_parallel_checkout(struct checkout *state);

int parallel_checkout__write_entry(const struct checkout *state,
				   struct cache_entry *ce);
int parallel_checkout__set_auto_write(const struct checkout *state);
int parallel_checkout__collect_results(const struct checkout *state);

#endif /* PARALLEL_CHECKOUT_H */
