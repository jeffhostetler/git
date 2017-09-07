#include "cache.h"
#include "oidset2.h"

static int oidset2_hashcmp(const void *unused_cmp_data,
			   const void *va, const void *vb,
			   const void *vkey)
{
	const struct oidset2_entry *a = va, *b = vb;
	const struct object_id *key = vkey;
	return oidcmp(&a->oid, key ? key : &b->oid);
}

struct oidset2_entry *oidset2_get(const struct oidset2 *set, const struct object_id *oid)
{
	struct hashmap_entry key;
	struct oidset2_entry *value;

	if (!set->map.cmpfn)
		return NULL;

	hashmap_entry_init(&key, sha1hash(oid->hash));
	value = hashmap_get(&set->map, &key, oid);

	return value;
}

int oidset2_contains(const struct oidset2 *set, const struct object_id *oid)
{
	return !!oidset2_get(set, oid);
}

int oidset2_insert(struct oidset2 *set, const struct object_id *oid,
		   enum object_type type, int64_t object_length,
		   const char *pathname)
{
	struct oidset2_entry *entry;

	if (!set->map.cmpfn)
		hashmap_init(&set->map, oidset2_hashcmp, NULL, 0);

	if (oidset2_contains(set, oid))
		return 1;

	entry = xcalloc(1, sizeof(*entry));
	hashmap_entry_init(&entry->hash, sha1hash(oid->hash));
	oidcpy(&entry->oid, oid);

	entry->type = type;
	entry->object_length = object_length;
	if (pathname)
	    entry->pathname = strdup(pathname);

	hashmap_add(&set->map, entry);
	return 0;
}

void oidset2_remove(struct oidset2 *set, const struct object_id *oid)
{
	struct hashmap_entry key;
	struct oidset2_entry *e;

	hashmap_entry_init(&key, sha1hash(oid->hash));
	e = hashmap_remove(&set->map, &key, oid);

	free(e->pathname);
	free(e);
}

void oidset2_clear(struct oidset2 *set)
{
	hashmap_free(&set->map, 1);
}

static int oidset2_cmp(const void *a, const void *b)
{
	const struct oidset2_entry *ae = *((const struct oidset2_entry **)a);
	const struct oidset2_entry *be = *((const struct oidset2_entry **)b);

	return oidcmp(&ae->oid, &be->oid);
}

void oidset2_foreach(struct oidset2 *set, oidset2_foreach_cb cb, void *cb_data)
{
	struct hashmap_iter iter;
	struct oidset2_entry **array;
	struct oidset2_entry *e;
	int j, k;

	array = xcalloc(set->map.size, sizeof(*e));

	hashmap_iter_init(&set->map, &iter);
	k = 0;
	while ((e = hashmap_iter_next(&iter)))
		array[k++] = e;

	QSORT(array, k, oidset2_cmp);

	for (j = 0; j < k; j++) {
		e = array[j];
		cb(j, k, e, cb_data);
	}

	free(array);
}
