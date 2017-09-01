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

/*
 * A filter for list-objects to omit ALL blobs from the traversal.
 * And to OPTIONALLY collect a list of the omitted OIDs.
 */
struct filter_blobs_none_data {
	struct oidset2 omits;
};

static list_objects_filter_result filter_blobs_none(
	list_objects_filter_type filter_type,
	struct object *obj,
	const char *pathname,
	const char *filename,
	void *filter_data_)
{
	struct filter_blobs_none_data *filter_data = filter_data_;
	unsigned long object_length;
	enum object_type t;

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

		/*
		 * Since we always omit all blobs (and never
		 * provisionally omit), we should never see
		 * a blob twice.
		 */
		assert(!oidset2_contains(&filter_data->omits, &obj->oid));

		t = sha1_object_info(obj->oid.hash, &object_length);
		if (t == OBJ_NONE) /* we may not have it locally */
			oidset2_insert_without_length(
				&filter_data->omits, &obj->oid,
				obj->type, pathname);
		else
			oidset2_insert(&filter_data->omits, &obj->oid,
				       obj->type, object_length,
				       pathname);

		return LOFR_MARK_SEEN; /* but not LOFR_SHOW (hard omit) */
	}
}

void traverse_commit_list__blobs_none(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data)
{
	struct filter_blobs_none_data d;

	memset(&d, 0, sizeof(d));

	traverse_commit_list_worker(revs, show_commit, show_object, ctx_data,
				    filter_blobs_none, &d);

	if (print_omitted_object)
		oidset2_foreach(&d.omits, print_omitted_object, ctx_data);

	oidset2_clear(&d.omits);
}
