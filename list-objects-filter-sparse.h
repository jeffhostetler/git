#ifndef LIST_OBJECTS_FILTERS_SPARSE_H
#define LIST_OBJECTS_FILTERS_SPARSE_H

#include "list-objects-filter-map.h"

/*
 * A filter driven by a sparse-checkout specification to only
 * include blobs that a sparse checkout would populate.
 *
 * The sparse-checkout spec can be loaded from a blob with the
 * given OID, a blob with a blob-ish path, or from a local pathname.
 * We allow an OID because the repo may be bare or we may be doing
 * the filtering on the server.
 */
void traverse_commit_list__sparse_oid(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	list_objects_filter_map_foreach_cb print_omitted_object,
	void *ctx_data,
	struct object_id *oid);
void traverse_commit_list__sparse_path(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	list_objects_filter_map_foreach_cb print_omitted_object,
	void *ctx_data,
	const char *path);

#endif /* LIST_OBJECTS_FILTERS_SPARSE_H */
