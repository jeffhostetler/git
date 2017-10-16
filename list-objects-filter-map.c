#include "cache.h"
#include "list-objects-filter-map.h"

void list_objects_filter_map_init(
	struct list_objects_filter_map *map, size_t initial_size)
{
	oidmap_init(&map->map, initial_size);
}

struct list_objects_filter_map_entry *list_objects_filter_map_get(
	const struct list_objects_filter_map *map,
	const struct object_id *oid)
{
	struct list_objects_filter_map_entry *e = oidmap_get(&map->map, oid);

	return e;
}

int list_objects_filter_map_contains(const struct list_objects_filter_map *map,
				     const struct object_id *oid)
{
	return !!list_objects_filter_map_get(map, oid);
}

int list_objects_filter_map_insert(struct list_objects_filter_map *map,
				   const struct object_id *oid,
				   const char *pathname, enum object_type type)
{
	struct list_objects_filter_map_entry *e;
	void *old;

	if (list_objects_filter_map_contains(map, oid))
		return 1;

	e = xcalloc(1, sizeof(*e));
	oidcpy(&e->entry.oid, oid);
	if (pathname && *pathname)
		e->pathname = strdup(pathname);
	e->type = type;

	old = oidmap_put(&map->map, e);
	assert(!old); /* since we already confirmed !contained */

	return 0;
}

static inline void lofme_free(struct list_objects_filter_map_entry *e)
{
	if (!e)
		return;
	if (e->pathname)
		free(e->pathname);
	free(e);
}

void list_objects_filter_map_remove(struct list_objects_filter_map *map,
				    const struct object_id *oid)
{
	struct list_objects_filter_map_entry *e;

	e = oidmap_remove(&map->map, oid);
	lofme_free(e);
}

void list_objects_filter_map_clear(struct list_objects_filter_map *map)
{
	struct hashmap_iter iter;
	struct list_objects_filter_map_entry *e;

	hashmap_iter_init(&map->map.map, &iter);
	while ((e = hashmap_iter_next(&iter)))
		lofme_free(e);

	oidmap_free(&map->map, 0);
}

static int my_cmp(const void *a, const void *b)
{
	const struct oidmap_entry *ea, *eb;

	ea = *(const struct oidmap_entry **)a;
	eb = *(const struct oidmap_entry **)b;

	return oidcmp(&ea->oid, &eb->oid);
}

void list_objects_filter_map_foreach(struct list_objects_filter_map *map,
				     list_objects_filter_map_foreach_cb cb,
				     void *cb_data)
{
	struct hashmap_iter iter;
	struct list_objects_filter_map_entry **array;
	struct list_objects_filter_map_entry *e;
	int k, nr;

	nr = hashmap_get_size(&map->map.map);
	if (!nr)
		return;

	array = xcalloc(nr, sizeof(*e));

	k = 0;
	hashmap_iter_init(&map->map.map, &iter);
	while ((e = hashmap_iter_next(&iter)))
		array[k++] = e;

	QSORT(array, nr, my_cmp);

	for (k = 0; k < nr; k++)
		cb(k, nr, array[k], cb_data);

	free(array);
}
