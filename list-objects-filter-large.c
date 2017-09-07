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
#include "list-objects-filter-large.h"

/*
 * A filter for list-objects to omit large blobs,
 * but always include ".git*" special files.
 */
struct filter_omit_large_blobs_data {
	struct oidset2 omits;
	int64_t max_bytes;
};

static list_objects_filter_result filter_omit_large_blobs(
	list_objects_filter_type filter_type,
	struct object *obj,
	const char *pathname,
	const char *filename,
	void *filter_data_)
{
	struct filter_omit_large_blobs_data *filter_data = filter_data_;
	int64_t object_length = -1;
	unsigned long s;
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
		 * If previously provisionally omitted (because of size), see if the
		 * current filename is special and force it to be included.
		 */
		if (oidset2_contains(&filter_data->omits, &obj->oid)) {
			if ((strncmp(filename, ".git", 4) == 0) && filename[4]) {
				oidset2_remove(&filter_data->omits, &obj->oid);
				return LOFR_MARK_SEEN | LOFR_SHOW;
			}
			return LOFR_ZERO; /* continue provisionally omitting it */
		}

		t = sha1_object_info(obj->oid.hash, &s);
		assert(t == OBJ_BLOB);
		object_length = (int64_t)((uint64_t)(s));

		if (object_length < filter_data->max_bytes)
			return LOFR_MARK_SEEN | LOFR_SHOW;

		/*
		 * Provisionally omit it.  We've already established that this blob
		 * is too big and doesn't have a special filename, so we WANT to
		 * omit it.  However, there may be a special file elsewhere in the
		 * tree that references this same blob, so we cannot reject it yet.
		 * Leave the LOFR_ bits unset so that if the blob appears again in
		 * the traversal, we will be asked again.
		 *
		 * No need for a pathname, since we only test for special filenames
		 * above.
		 */
		oidset2_insert(&filter_data->omits, &obj->oid, t, object_length,
			       NULL);
		return LOFR_ZERO;
	}
}

void traverse_commit_list_omit_large_blobs(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data,
	int64_t large_byte_limit)
{
	struct filter_omit_large_blobs_data d;

	memset(&d, 0, sizeof(d));
	d.max_bytes = large_byte_limit;

	traverse_commit_list_worker(revs, show_commit, show_object, ctx_data,
				    filter_omit_large_blobs, &d);

	if (print_omitted_object)
		oidset2_foreach(&d.omits, print_omitted_object, ctx_data);

	oidset2_clear(&d.omits);
}
