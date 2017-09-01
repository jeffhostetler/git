#ifndef LIST_OBJECTS_FILTER_BLOBS_NONE_H
#define LIST_OBJECTS_FILTER_BLOBS_NONE_H

#include "list-objects-filter-map.h"

/*
 * A filter for list-objects to omit ALL blobs
 * from the traversal.
 */
void traverse_commit_list__blobs_none(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	list_objects_filter_map_foreach_cb print_omitted_object,
	void *ctx_data);

#endif /* LIST_OBJECTS_FILTER_BLOBS_NONE_H */

