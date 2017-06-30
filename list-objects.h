#ifndef LIST_OBJECTS_H
#define LIST_OBJECTS_H

typedef void (*show_commit_fn)(struct commit *, void *);
typedef void (*show_object_fn)(struct object *, const char *, void *);
void traverse_commit_list(struct rev_info *, show_commit_fn, show_object_fn, void *);

typedef void (*show_edge_fn)(struct commit *);
void mark_edges_uninteresting(struct rev_info *, show_edge_fn);

enum list_objects_filter_result {
	LOFR_ZERO      = 0,
	LOFR_MARK_SEEN = 1<<0,
	LOFR_SHOW      = 1<<1,
};

/* See object.h and revision.h */
#define FILTER_REVISIT (1<<25)

enum list_objects_filter_type {
	LOFT_BEGIN_TREE,
	LOFT_END_TREE,
	LOFT_BLOB
};

typedef enum list_objects_filter_result list_objects_filter_result;
typedef enum list_objects_filter_type list_objects_filter_type;

typedef list_objects_filter_result (*filter_object_fn)(
	list_objects_filter_type filter_type,
	struct object *obj,
	const char *pathname,
	const char *filename,
	void *filter_data);

void traverse_commit_list_worker(
	struct rev_info *,
	show_commit_fn, show_object_fn, void *show_data,
	filter_object_fn filter, void *filter_data);

#endif
