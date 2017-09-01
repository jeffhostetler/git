#ifndef LIST_OBJECTS_FILTER_ALL_H
#define LIST_OBJECTS_FILTER_ALL_H

#include "oidset2.h"

/*
 * A filter for list-objects to omit ALL blobs
 * from the traversal.
 */
void traverse_commit_list_omit_all_blobs(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data);

#endif /* LIST_OBJECTS_FILTER_ALL_H */

