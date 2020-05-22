#ifndef PARALLEL_CHECKOUT_HELPER_H
#define PARALLEL_CHECKOUT_HELPER_H

#include "cache.h"

/*
 * After attempting to populate a blob in the worktree,
 * `git-parallel-checkout-helper` reports the individual
 * operation that failed back to the foreground process.
 *
 * Conceptually, this is half of a `(<class>, <errno>)` pair.
 *
 * TODO decide whether IEC__OK should be zero for tradition.
 * TODO see xcalloc() in parallel-checkout.c
 */
enum parallel_checkout_helper__item_error_class {
	IEC__NO_RESULT = 0, /* no result from helper process (yet) */
	IEC__INVALID_ITEM, /* helper does not know about this item */
	IEC__OK,
	IEC__LOAD, /* helper could not load blob into memory */
	IEC__OPEN, /* helper could not create the file, see item_errno */
	IEC__WRITE,
	IEC__LSTAT,
};

/*
 * The record format shared between `parallel-checkout-client.c` and
 * the `git-parallel-checkout-helper` process to request a blob to be
 * queued for population in the worktree.
 *
 * This is the fixed portion of the record.  Following it will be 2
 * strings: the encoding and the pathname.  These are NOT null
 * terminated (since we have the exact lengths in the fixed-portion of
 * the record).
 */
struct parallel_checkout_helper__queue_item_record {
	uint32_t pc_item_nr;
	uint32_t helper_item_nr;
	uint32_t ce_mode;
	uint32_t attr_action;
	uint32_t crlf_action;
	uint32_t ident;
	uint32_t len_name;
	uint32_t len_encoding_name;
	struct object_id oid;
};

#endif /* PARALLEL_CHECKOUT_HELPER_H */
