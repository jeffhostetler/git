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
#include "list-objects-filter-blobs-limit.h"

#define DEFAULT_MAP_SIZE (16*1024)

/*
 * A filter for list-objects to omit large blobs,
 * but always include ".git*" special files.
 * And to OPTIONALLY collect a list of the omitted OIDs.
 */
struct filter_blobs_limit_data {
	struct oidmap *omits;
	unsigned long max_bytes;
};

static list_objects_filter_result filter_blobs_limit(
	list_objects_filter_type filter_type,
	struct object *obj,
	const char *pathname,
	const char *filename,
	void *filter_data_)
{
	struct filter_blobs_limit_data *filter_data = filter_data_;
	struct list_objects_filter_data_entry *entry;
	unsigned long object_length;
	enum object_type t;
	int is_special_filename;

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

		is_special_filename = ((strncmp(filename, ".git", 4) == 0) &&
				       filename[4]);

		/*
		 * If we are keeping a list of the omitted objects
		 * for the caller *AND* we previously "provisionally"
		 * omitted this object (because of size) *AND* it now
		 * has a special filename, make it not-omitted.
		 * Otherwise, continue to provisionally omit it.
		 */
		if (filter_data->omits &&
		    oidmap_get(filter_data->omits, &obj->oid)) {
			if (!is_special_filename)
				return LOFR_ZERO;
			entry = oidmap_remove(filter_data->omits, &obj->oid);
			free(entry);
			return LOFR_MARK_SEEN | LOFR_SHOW;
		}

		/*
		 * If filename matches ".git*", always include it (regardless
		 * of size).  (This may include blobs that we do not have
		 * locally.)
		 */
		if (is_special_filename)
			return LOFR_MARK_SEEN | LOFR_SHOW;

		t = sha1_object_info(obj->oid.hash, &object_length);
		if (t != OBJ_BLOB) { /* probably OBJ_NONE */
			/*
			 * We DO NOT have the blob locally, so we cannot
			 * apply the size filter criteria.  Be conservative
			 * and force show it (and let the caller deal with
			 * the ambiguity).  (This matches the behavior above
			 * when the special filename matches.)
			 */
			return LOFR_MARK_SEEN | LOFR_SHOW;
		}

		if (object_length < filter_data->max_bytes)
			return LOFR_MARK_SEEN | LOFR_SHOW;

		/*
		 * Provisionally omit it.  We've already established
		 * that this blob is too big and doesn't have a special
		 * filename, so we *WANT* to omit it.  However, there
		 * may be a special file elsewhere in the tree that
		 * references this same blob, so we cannot reject it
		 * just yet.  Leave the LOFR_ bits unset so that *IF*
		 * the blob appears again in the traversal, we will
		 * be asked again.
		 *
		 * If we are keeping a list of the ommitted objects,
		 * provisionally add it to the list.
		 */

		if (filter_data->omits)
			list_objects_filter_map_insert(filter_data->omits,
						       &obj->oid, pathname,
						       obj->type);

		return LOFR_ZERO;
	}
}

void traverse_commit_list__blobs_limit(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	list_objects_filter_map_foreach_cb print_omitted_object,
	void *ctx_data,
	unsigned long large_byte_limit)
{
	struct filter_blobs_limit_data d;

	memset(&d, 0, sizeof(d));
	if (print_omitted_object) {
		d.omits = xcalloc(1, sizeof(*d.omits));
		oidmap_init(d.omits, DEFAULT_MAP_SIZE);
	}
	d.max_bytes = large_byte_limit;

	traverse_commit_list_worker(revs, show_commit, show_object, ctx_data,
				    filter_blobs_limit, &d);

	if (print_omitted_object) {
		list_objects_filter_map_foreach(d.omits, print_omitted_object,
						ctx_data);
		oidmap_free(d.omits, 1);
	}
}
