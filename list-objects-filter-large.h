#ifndef LIST_OBJECTS_FILTER_LARGE_H
#define LIST_OBJECTS_FILTER_LARGE_H

#include "oidset2.h"

/*
 * A filter for list-objects to omit large blobs,
 * but always include ".git*" special files.
 */
void traverse_commit_list_omit_large_blobs(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data,
	int64_t large_byte_limit);

#endif /* LIST_OBJECTS_FILTER_LARGE_H */
