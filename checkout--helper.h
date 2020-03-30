#ifndef CHECKOUT_HELPER_H
#define CHECKOUT_HELPER_H

/*
 * Record format used by parallel-checkout.c to describe in item
 * when queuing it to a checkout--helper process.  This is the
 * fixed portion of the record.  Following it will be 2 strings:
 * the encoding and the pathname.
 */
struct checkout_helper__queue_item_record {
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

/*
 * Allow writer thread(s) in checkout--helper to automatically
 * write files into the worktree as soon as the necessary blobs
 * are loaded into memory by the preload thread.
 */
#define CHECKOUT_HELPER__AUTO_WRITE maximum_signed_value_of_type(int)

/*
 * The individual operation that failed within a `checkout--helper`
 * request.
 *
 * Conceptually, this is half of a { <class>, <errno> } tuple.
 */
enum checkout_helper__item_error_class {
	IEC__NO_RESULT = 0, /* no result from helper process (yet) */
	IEC__INVALID_ITEM, /* helper does not know about this item */
	IEC__OK,
	IEC__LOAD,
	IEC__OPEN, /* helper could not create the file, see item_errno */
	IEC__WRITE,
	IEC__LSTAT,
};

/*
 * `checkout--helper` can aysnchronously load the blob associated
 * with an OID into memory, smudge it, write it to a requested
 * pathname in the worktree, lstat() the new file, and later
 * respond to the foreground process with the aggregate result.
 *
 * This is the fixed-width fixed-field response from `checkout--helper`
 * for a single item.
 *
 * Encode the enum fields as fixed-width integers for portability
 * and alignment.
 *
 * See `sync_get1`.
 */
struct checkout_helper__item_result {
	uint32_t pc_item_nr;
	uint32_t helper_item_nr;
	uint32_t item_error_class; /* enum checkout_helper__item_error_class */
	uint32_t item_errno;
	struct stat st;
};

#endif /* CHECKOUT_HELPER_H */
