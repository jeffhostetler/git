#ifndef LIST_OBJECTS_FILTER_BLOBS_NONE_H
#define LIST_OBJECTS_FILTER_BLOBS_NONE_H

#include "oidset2.h"

/*
 * A filter for list-objects to omit ALL blobs
 * from the traversal.
 */
void traverse_commit_list__blobs_none(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data);

#endif /* LIST_OBJECTS_FILTER_BLOBS_NONE_H */

