#ifndef PARALLEL_CHECKOUT_H
#define PARALLEL_CHECKOUT_H

enum parallel_checkout_cmd_result {
	PCCR_OK = 0,

	PCCR_UNKNOWN_FIELD,
	PCCR_MISSING_REQUIRED_FIELD,

	PCCR_REQUEST_OUT_OF_ORDER,

	PCCR_BLOB_ERROR,

	PCCR_OPEN_ERROR,
	PCCR_WRITE_ERROR,
};

int is_eligible_for_parallel_checkout(const struct conv_attrs *ca);

void setup_parallel_checkout(struct checkout *state);
void finish_parallel_checkout(struct checkout *state);

int parallel_checkout_write_entry(const struct checkout *state,
				  struct cache_entry *ce);

#endif /* PARALLEL_CHECKOUT_H */
