#include "cache.h"
#include "dir.h"
#include "tag.h"
#include "commit.h"
#include "tree.h"
#include "blob.h"
#include "diff.h"
#include "tree-walk.h"
#include "revision.h"
#include "list-objects.h"
#include "list-objects-filter-blobs-none.h"

#define DEFAULT_MAP_SIZE (16*1024)

/*
 * A filter for list-objects to omit ALL blobs from the traversal.
 * And to OPTIONALLY collect a list of the omitted OIDs.
 */
struct filter_blobs_none_data {
	struct oidmap *omits;
};

static list_objects_filter_result filter_blobs_none(
	list_objects_filter_type filter_type,
	struct object *obj,
	const char *pathname,
	const char *filename,
	void *filter_data_)
{
	struct filter_blobs_none_data *filter_data = filter_data_;

	switch (filter_type) {
	default:
		die("unkown filter_type");
		return LOFR_ZERO;

	case LOFT_BEGIN_TREE:
		assert(obj->type == OBJ_TREE);
		/* always include all tree objects */
		return LOFR_MARK_SEEN | LOFR_SHOW;

	case LOFT_END_TREE:
		assert(obj->type == OBJ_TREE);
		return LOFR_ZERO;

	case LOFT_BLOB:
		assert(obj->type == OBJ_BLOB);
		assert((obj->flags & SEEN) == 0);

		if (filter_data->omits)
			list_objects_filter_map_insert(
				filter_data->omits, &obj->oid, pathname,
				obj->type);

		return LOFR_MARK_SEEN; /* but not LOFR_SHOW (hard omit) */
	}
}

void traverse_commit_list__blobs_none(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	list_objects_filter_map_foreach_cb print_omitted_object,
	void *ctx_data)
{
	struct filter_blobs_none_data d;

	memset(&d, 0, sizeof(d));
	if (print_omitted_object) {
		d.omits = xcalloc(1, sizeof(*d.omits));
		oidmap_init(d.omits, DEFAULT_MAP_SIZE);
	}

	traverse_commit_list_worker(revs, show_commit, show_object, ctx_data,
				    filter_blobs_none, &d);

	if (print_omitted_object) {
		list_objects_filter_map_foreach(d.omits,
						print_omitted_object,
						ctx_data);
		oidmap_free(d.omits, 1);
	}
}
