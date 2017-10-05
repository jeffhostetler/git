#ifndef OIDSET2_H
#define OIDSET2_H

/**
 * oidset2 is a variant of oidset, but allows additional fields for each object.
 */

/**
 * A single oidset2; should be zero-initialized (or use OIDSET2_INIT).
 */
struct oidset2 {
	struct hashmap map;
};

#define OIDSET2_INIT { { NULL } }

struct oidset2_entry {
	struct hashmap_entry hash;
	struct object_id oid;

	enum object_type type;
	int64_t object_length;	/* This is SIGNED. Use -1 when unknown. */
	char *pathname;
};

struct oidset2_entry *oidset2_get(const struct oidset2 *set, const struct object_id *oid);

/**
 * Returns true iff `set` contains `oid`.
 */
int oidset2_contains(const struct oidset2 *set, const struct object_id *oid);

/**
 * Insert the oid into the set; a copy is made, so "oid" does not need
 * to persist after this function is called.
 *
 * Returns 1 if the oid was already in the set, 0 otherwise. This can be used
 * to perform an efficient check-and-add.
 */
int oidset2_insert(struct oidset2 *set, const struct object_id *oid,
		   enum object_type type, int64_t object_length,
		   const char *pathname);

void oidset2_remove(struct oidset2 *set, const struct object_id *oid);

typedef void (*oidset2_foreach_cb)(
	int i, int i_limit,
	struct oidset2_entry *e, void *cb_data);

void oidset2_foreach(struct oidset2 *set, oidset2_foreach_cb cb, void *cb_data);

/**
 * Remove all entries from the oidset2, freeing any resources associated with
 * it.
 */
void oidset2_clear(struct oidset2 *set);

#endif /* OIDSET2_H */
