#ifndef LIST_OBJECTS_FILTER_MAP_H
#define LIST_OBJECTS_FILTER_MAP_H

#include "oidmap.h"

struct list_objects_filter_map {
	struct oidmap map;
};

#define LIST_OBJECTS_FILTER_MAP_INIT { { NULL } }

struct list_objects_filter_map_entry {
	struct oidmap_entry entry; /* must be first */

	char *pathname;
	enum object_type type;
};

extern void list_objects_filter_map_init(
	struct list_objects_filter_map *map, size_t initial_size);

extern struct list_objects_filter_map_entry *list_objects_filter_map_get(
	const struct list_objects_filter_map *map,
	const struct object_id *oid);

extern int list_objects_filter_map_contains(
	const struct list_objects_filter_map *map,
	const struct object_id *oid);

extern int list_objects_filter_map_insert(
	struct list_objects_filter_map *map,
	const struct object_id *oid,
	const char *pathname, enum object_type type);

extern void list_objects_filter_map_remove(
	struct list_objects_filter_map *map,
	const struct object_id *oid);

extern void list_objects_filter_map_clear(struct list_objects_filter_map *map);

typedef void (*list_objects_filter_map_foreach_cb)(
	int i, int i_limit,
	struct list_objects_filter_map_entry *e, void *cb_data);

extern void list_objects_filter_map_foreach(
	struct list_objects_filter_map *map,
	list_objects_filter_map_foreach_cb cb,
	void *cb_data);

#endif /* LIST_OBJECTS_FILTER_MAP_H */
