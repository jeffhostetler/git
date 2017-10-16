#include "cache.h"
#include "list-objects-filter-map.h"

int list_objects_filter_map_insert(struct oidmap *map,
				   const struct object_id *oid,
				   const char *pathname, enum object_type type)
{
	size_t len, size;
	struct list_objects_filter_map_entry *e;

	if (oidmap_get(map, oid))
		return 1;

	len = ((pathname && *pathname) ? strlen(pathname) : 0);
	size = (offsetof(struct list_objects_filter_map_entry, pathname) + len + 1);
	e = xcalloc(1, size);

	oidcpy(&e->entry.oid, oid);
	e->type = type;
	if (pathname && *pathname)
		strcpy(e->pathname, pathname);

	oidmap_put(map, e);
	return 0;
}

static int my_cmp(const void *a, const void *b)
{
	const struct oidmap_entry *ea, *eb;

	ea = *(const struct oidmap_entry **)a;
	eb = *(const struct oidmap_entry **)b;

	return oidcmp(&ea->oid, &eb->oid);
}

void list_objects_filter_map_foreach(struct oidmap *map,
				     list_objects_filter_map_foreach_cb cb,
				     void *cb_data)
{
	struct hashmap_iter iter;
	struct list_objects_filter_map_entry **array;
	struct list_objects_filter_map_entry *e;
	int k, nr;

	nr = hashmap_get_size(&map->map);
	if (!nr)
		return;

	array = xcalloc(nr, sizeof(*e));

	k = 0;
	hashmap_iter_init(&map->map, &iter);
	while ((e = hashmap_iter_next(&iter)))
		array[k++] = e;

	QSORT(array, nr, my_cmp);

	for (k = 0; k < nr; k++)
		cb(k, nr, array[k], cb_data);

	free(array);
}
