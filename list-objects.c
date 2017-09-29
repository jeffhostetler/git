#include "cache.h"
#include "tag.h"
#include "commit.h"
#include "tree.h"
#include "blob.h"
#include "diff.h"
#include "tree-walk.h"
#include "revision.h"
#include "list-objects.h"
#include "list-objects-filter-all.h"
#include "list-objects-filter-large.h"
#include "list-objects-filter-sparse.h"

static void process_blob(struct rev_info *revs,
			 struct blob *blob,
			 show_object_fn show,
			 struct strbuf *path,
			 const char *name,
			 void *cb_data,
			 filter_object_fn filter,
			 void *filter_data)
{
	struct object *obj = &blob->object;
	size_t pathlen;
	list_objects_filter_result r = LOFR_MARK_SEEN | LOFR_SHOW;

	if (!revs->blob_objects)
		return;
	if (!obj)
		die("bad blob object");
	if (obj->flags & (UNINTERESTING | SEEN))
		return;

	pathlen = path->len;
	strbuf_addstr(path, name);
	if (filter)
		r = filter(LOFT_BLOB, obj, path->buf, &path->buf[pathlen], filter_data);
	if (r & LOFR_MARK_SEEN)
		obj->flags |= SEEN;
	if (r & LOFR_SHOW)
		show(obj, path->buf, cb_data);
	strbuf_setlen(path, pathlen);
}

/*
 * Processing a gitlink entry currently does nothing, since
 * we do not recurse into the subproject.
 *
 * We *could* eventually add a flag that actually does that,
 * which would involve:
 *  - is the subproject actually checked out?
 *  - if so, see if the subproject has already been added
 *    to the alternates list, and add it if not.
 *  - process the commit (or tag) the gitlink points to
 *    recursively.
 *
 * However, it's unclear whether there is really ever any
 * reason to see superprojects and subprojects as such a
 * "unified" object pool (potentially resulting in a totally
 * humongous pack - avoiding which was the whole point of
 * having gitlinks in the first place!).
 *
 * So for now, there is just a note that we *could* follow
 * the link, and how to do it. Whether it necessarily makes
 * any sense what-so-ever to ever do that is another issue.
 */
static void process_gitlink(struct rev_info *revs,
			    const unsigned char *sha1,
			    show_object_fn show,
			    struct strbuf *path,
			    const char *name,
			    void *cb_data)
{
	/* Nothing to do */
}

static void process_tree(struct rev_info *revs,
			 struct tree *tree,
			 show_object_fn show,
			 struct strbuf *base,
			 const char *name,
			 void *cb_data,
			 filter_object_fn filter,
			 void *filter_data)
{
	struct object *obj = &tree->object;
	struct tree_desc desc;
	struct name_entry entry;
	enum interesting match = revs->diffopt.pathspec.nr == 0 ?
		all_entries_interesting: entry_not_interesting;
	int baselen = base->len;
	list_objects_filter_result r = LOFR_MARK_SEEN | LOFR_SHOW;

	if (!revs->tree_objects)
		return;
	if (!obj)
		die("bad tree object");
	if (obj->flags & (UNINTERESTING | SEEN))
		return;
	if (parse_tree_gently(tree, revs->ignore_missing_links) < 0) {
		if (revs->ignore_missing_links)
			return;
		die("bad tree object %s", oid_to_hex(&obj->oid));
	}

	strbuf_addstr(base, name);
	if (filter)
		r = filter(LOFT_BEGIN_TREE, obj, base->buf, &base->buf[baselen], filter_data);
	if (r & LOFR_MARK_SEEN)
		obj->flags |= SEEN;
	if (r & LOFR_SHOW)
		show(obj, base->buf, cb_data);
	if (base->len)
		strbuf_addch(base, '/');

	init_tree_desc(&desc, tree->buffer, tree->size);

	while (tree_entry(&desc, &entry)) {
		if (match != all_entries_interesting) {
			match = tree_entry_interesting(&entry, base, 0,
						       &revs->diffopt.pathspec);
			if (match == all_entries_not_interesting)
				break;
			if (match == entry_not_interesting)
				continue;
		}

		if (S_ISDIR(entry.mode))
			process_tree(revs,
				     lookup_tree(entry.oid),
				     show, base, entry.path,
				     cb_data, filter, filter_data);
		else if (S_ISGITLINK(entry.mode))
			process_gitlink(revs, entry.oid->hash,
					show, base, entry.path,
					cb_data);
		else
			process_blob(revs,
				     lookup_blob(entry.oid),
				     show, base, entry.path,
				     cb_data, filter, filter_data);
	}

	if (filter) {
		r = filter(LOFT_END_TREE, obj, base->buf, &base->buf[baselen], filter_data);
		if (r & LOFR_MARK_SEEN)
			obj->flags |= SEEN;
		if (r & LOFR_SHOW)
			show(obj, base->buf, cb_data);
	}

	strbuf_setlen(base, baselen);
	free_tree_buffer(tree);
}

static void mark_edge_parents_uninteresting(struct commit *commit,
					    struct rev_info *revs,
					    show_edge_fn show_edge)
{
	struct commit_list *parents;

	for (parents = commit->parents; parents; parents = parents->next) {
		struct commit *parent = parents->item;
		if (!(parent->object.flags & UNINTERESTING))
			continue;
		mark_tree_uninteresting(parent->tree);
		if (revs->edge_hint && !(parent->object.flags & SHOWN)) {
			parent->object.flags |= SHOWN;
			show_edge(parent);
		}
	}
}

void mark_edges_uninteresting(struct rev_info *revs, show_edge_fn show_edge)
{
	struct commit_list *list;
	int i;

	for (list = revs->commits; list; list = list->next) {
		struct commit *commit = list->item;

		if (commit->object.flags & UNINTERESTING) {
			mark_tree_uninteresting(commit->tree);
			if (revs->edge_hint_aggressive && !(commit->object.flags & SHOWN)) {
				commit->object.flags |= SHOWN;
				show_edge(commit);
			}
			continue;
		}
		mark_edge_parents_uninteresting(commit, revs, show_edge);
	}
	if (revs->edge_hint_aggressive) {
		for (i = 0; i < revs->cmdline.nr; i++) {
			struct object *obj = revs->cmdline.rev[i].item;
			struct commit *commit = (struct commit *)obj;
			if (obj->type != OBJ_COMMIT || !(obj->flags & UNINTERESTING))
				continue;
			mark_tree_uninteresting(commit->tree);
			if (!(obj->flags & SHOWN)) {
				obj->flags |= SHOWN;
				show_edge(commit);
			}
		}
	}
}

static void add_pending_tree(struct rev_info *revs, struct tree *tree)
{
	add_pending_object(revs, &tree->object, "");
}

void traverse_commit_list_worker(
	struct rev_info *revs,
	show_commit_fn show_commit, show_object_fn show_object, void *show_data,
	filter_object_fn filter, void *filter_data)
{
	int i;
	struct commit *commit;
	struct strbuf base;

	strbuf_init(&base, PATH_MAX);
	while ((commit = get_revision(revs)) != NULL) {
		/*
		 * an uninteresting boundary commit may not have its tree
		 * parsed yet, but we are not going to show them anyway
		 */
		if (commit->tree)
			add_pending_tree(revs, commit->tree);
		show_commit(commit, show_data);
	}
	for (i = 0; i < revs->pending.nr; i++) {
		struct object_array_entry *pending = revs->pending.objects + i;
		struct object *obj = pending->item;
		const char *name = pending->name;
		const char *path = pending->path;
		if (obj->flags & (UNINTERESTING | SEEN))
			continue;
		if (obj->type == OBJ_TAG) {
			obj->flags |= SEEN;
			show_object(obj, name, show_data);
			continue;
		}
		if (!path)
			path = "";
		if (obj->type == OBJ_TREE) {
			process_tree(revs, (struct tree *)obj, show_object,
				     &base, path, show_data, filter, filter_data);
			continue;
		}
		if (obj->type == OBJ_BLOB) {
			process_blob(revs, (struct blob *)obj, show_object,
				     &base, path, show_data, filter, filter_data);
			continue;
		}
		die("unknown pending object %s (%s)",
		    oid_to_hex(&obj->oid), name);
	}
	object_array_clear(&revs->pending);
	strbuf_release(&base);
}

void traverse_commit_list(struct rev_info *revs,
			  show_commit_fn show_commit,
			  show_object_fn show_object,
			  void *show_data)
{
	traverse_commit_list_worker(
		revs,
		show_commit, show_object, show_data,
		NULL, NULL);
}

void traverse_commit_list_filtered(
	struct object_filter_options *filter_options,
	struct rev_info *revs,
	show_commit_fn show_commit,
	show_object_fn show_object,
	oidset2_foreach_cb print_omitted_object,
	void *show_data)
{
	if (filter_options->omit_all_blobs)
		traverse_commit_list_omit_all_blobs(
			revs, show_commit, show_object, print_omitted_object, show_data);

	else if (filter_options->omit_large_blobs)
		traverse_commit_list_omit_large_blobs(
			revs, show_commit, show_object, print_omitted_object, show_data,
			(int64_t)(uint64_t)filter_options->large_byte_limit);

	else if (filter_options->use_blob)
		traverse_commit_list_use_blob(
			revs, show_commit, show_object, print_omitted_object, show_data,
			&filter_options->sparse_oid);

	else if (filter_options->use_path)
		traverse_commit_list_use_path(
			revs, show_commit, show_object, print_omitted_object, show_data,
			filter_options->sparse_value);

	else
		die("unspecified list-objects filter");
}
