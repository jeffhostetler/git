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
#include "list-objects-filter-sparse.h"

/*
 * A filter driven by a sparse-checkout specification to only
 * include blobs that a sparse checkout would populate.
 *
 * The sparse-checkout spec can be loaded from a blob with the
 * given OID or from a local pathname.  We allow an OID because
 * the repo may be bare or we may be doing the filtering on the
 * server.
 */
struct frame {
	int defval;
	int child_prov_omit : 1;
};

struct filter_use_sparse_data {
	struct oidset2 omits;
	struct exclude_list el;

	size_t nr, alloc;
	struct frame *array_frame;
};

static list_objects_filter_result filter_use_sparse(
	list_objects_filter_type filter_type,
	struct object *obj,
	const char *pathname,
	const char *filename,
	void *filter_data_)
{
	struct filter_use_sparse_data *filter_data = filter_data_;
	int64_t object_length = -1;
	int val, dtype;
	unsigned long s;
	enum object_type t;
	struct frame *frame;

	switch (filter_type) {
	default:
		die("unkown filter_type");
		return LOFR_ZERO;

	case LOFT_BEGIN_TREE:
		assert(obj->type == OBJ_TREE);
		dtype = DT_DIR;
		val = is_excluded_from_list(pathname, strlen(pathname),
					    filename, &dtype, &filter_data->el,
					    &the_index);
		if (val < 0)
			val = filter_data->array_frame[filter_data->nr].defval;

		ALLOC_GROW(filter_data->array_frame, filter_data->nr + 1,
			   filter_data->alloc);
		filter_data->nr++;
		filter_data->array_frame[filter_data->nr].defval = val;
		filter_data->array_frame[filter_data->nr].child_prov_omit = 0;

		/*
		 * A directory with this tree OID may appear in multiple
		 * places in the tree. (Think of a directory move, with
		 * no other changes.)  And with a different pathname, the
		 * is_excluded...() results for this directory and items
		 * contained within it may be different.  So we cannot
		 * mark it SEEN (yet), since that will prevent process_tree()
		 * from revisiting this tree object with other pathnames.
		 *
		 * Only SHOW the tree object the first time we visit this
		 * tree object.
		 *
		 * We always show all tree objects.  A future optimization
		 * may want to attempt to narrow this.
		 */
		if (obj->flags & FILTER_REVISIT)
			return LOFR_ZERO;
		obj->flags |= FILTER_REVISIT;
		return LOFR_SHOW;

	case LOFT_END_TREE:
		assert(obj->type == OBJ_TREE);
		assert(filter_data->nr > 0);

		frame = &filter_data->array_frame[filter_data->nr];
		filter_data->nr--;

		/*
		 * Tell our parent directory if any of our children were
		 * provisionally omitted.
		 */
		filter_data->array_frame[filter_data->nr].child_prov_omit |=
			frame->child_prov_omit;

		/*
		 * If there are NO provisionally omitted child objects (ALL child
		 * objects in this folder were INCLUDED), then we can mark the
		 * folder as SEEN (so we will not have to revisit it again).
		 */
		if (!frame->child_prov_omit)
			return LOFR_MARK_SEEN;
		return LOFR_ZERO;

	case LOFT_BLOB:
		assert(obj->type == OBJ_BLOB);
		assert((obj->flags & SEEN) == 0);

		frame = &filter_data->array_frame[filter_data->nr];

		/*
		 * If we previously provisionally omitted this blob because
		 * its pathname was not in the sparse-checkout AND this
		 * reference to the blob has the same pathname, we can avoid
		 * repeating the exclusion logic on this pathname and just
		 * continue to provisionally omit it.
		 */
		if (obj->flags & FILTER_REVISIT) {
			struct oidset2_entry *entry_prev;
			entry_prev = oidset2_get(&filter_data->omits, &obj->oid);
			if (entry_prev && !strcmp(pathname, entry_prev->pathname)) {
				frame->child_prov_omit = 1;
				return LOFR_ZERO;
			}
		}

		dtype = DT_REG;
		val = is_excluded_from_list(pathname, strlen(pathname),
					    filename, &dtype, &filter_data->el,
					    &the_index);
		if (val < 0)
			val = frame->defval;
		if (val > 0)
			return LOFR_MARK_SEEN | LOFR_SHOW;

		t = sha1_object_info(obj->oid.hash, &s);
		assert(t == OBJ_BLOB);
		object_length = (int64_t)((uint64_t)(s));

		/*
		 * Provisionally omit it.  We've already established that
		 * this pathname is not in the sparse-checkout specification,
		 * so we WANT to omit this blob.  However, a pathname elsewhere
		 * in the tree may also reference this same blob, so we cannot
		 * reject it yet.  Leave the LOFR_ bits unset so that if the
		 * blob appears again in the traversal, we will be asked again.
		 *
		 * The pathname we associate with this omit is just the first
		 * one we saw for this blob.  Other instances of this blob may
		 * have other pathnames and that is fine.  We just use it for
		 * perf because most of the time, the blob will be in the same
		 * place as we walk the commits.
		 */
		oidset2_insert(&filter_data->omits, &obj->oid, t, object_length,
			       pathname);
		obj->flags |= FILTER_REVISIT;
		frame->child_prov_omit = 1;
		return LOFR_ZERO;
	}
}

void traverse_commit_list_use_blob(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data,
	struct object_id *oid)
{
	struct filter_use_sparse_data d;

	memset(&d, 0, sizeof(d));
	if (add_excludes_from_blob_to_list(oid, NULL, 0, &d.el) < 0)
		die("could not load filter specification");

	ALLOC_GROW(d.array_frame, d.nr + 1, d.alloc);
	d.array_frame[d.nr].defval = 0; /* default to include */
	d.array_frame[d.nr].child_prov_omit = 0;

	traverse_commit_list_worker(revs, show_commit, show_object, ctx_data,
				    filter_use_sparse, &d);

	if (print_omitted_object)
		oidset2_foreach(&d.omits, print_omitted_object, ctx_data);

	oidset2_clear(&d.omits);
}

void traverse_commit_list_use_path(
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *ctx_data,
	const char *path)
{
	struct filter_use_sparse_data d;

	memset(&d, 0, sizeof(d));
	if (add_excludes_from_file_to_list(path, NULL, 0, &d.el, NULL) < 0)
		die("could not load filter specification");

	ALLOC_GROW(d.array_frame, d.nr + 1, d.alloc);
	d.array_frame[d.nr].defval = 0; /* default to include */
	d.array_frame[d.nr].child_prov_omit = 0;

	traverse_commit_list_worker(revs, show_commit, show_object, ctx_data,
				    filter_use_sparse, &d);

	if (print_omitted_object)
		oidset2_foreach(&d.omits, print_omitted_object, ctx_data);

	oidset2_clear(&d.omits);
}
