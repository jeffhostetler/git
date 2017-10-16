#ifndef LIST_OBJECTS_FILTER_MAP_H
#define LIST_OBJECTS_FILTER_MAP_H

#include "oidmap.h"

struct list_objects_filter_map_entry {
	struct oidmap_entry entry; /* must be first */
	enum object_type type;
	char pathname[FLEX_ARRAY];
};

extern int list_objects_filter_map_insert(
	struct oidmap *map,
	const struct object_id *oid,
	const char *pathname, enum object_type type);

typedef void (*list_objects_filter_map_foreach_cb)(
	int i, int i_limit,
	struct list_objects_filter_map_entry *e, void *cb_data);

extern void list_objects_filter_map_foreach(
	struct oidmap *map,
	list_objects_filter_map_foreach_cb cb,
	void *cb_data);

#endif /* LIST_OBJECTS_FILTER_MAP_H */
