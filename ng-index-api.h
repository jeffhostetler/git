#ifndef NG_INDEX_API_H
#define NG_INDEX_API_H

/*
 * A temporary solution to let us phase out the original Index API macros.
 *
 * As we convert more macro references and more source files, we remove more
 * and more macros, but unconverted source files still see the full set defined
 * in cache.h.
 */
#ifndef NO_THE_INDEX_COMPATIBILITY_MACROS
#undef active_cache
#undef active_nr
#undef active_alloc
#undef active_cache_changed
#undef active_cache_tree

#undef read_cache
#undef read_cache_from
#undef read_cache_preload
#undef is_cache_unborn
#undef read_cache_unmerged
#undef discard_cache
#undef unmerged_cache
#undef cache_name_pos
#undef add_cache_entry
#undef rename_cache_entry_at
#undef remove_cache_entry_at
#undef remove_file_from_cache
#undef add_to_cache
#undef add_file_to_cache
#undef chmod_cache_entry
#undef refresh_cache
#undef ce_match_stat
#undef ce_modified
#undef cache_dir_exists
#undef cache_file_exists
#undef cache_name_is_other
#undef resolve_undo_clear
#undef unmerge_cache_entry_at
#undef unmerge_cache
#undef read_blob_data_from_cache
#endif


/*
 * We DO NOT define _INIT macros for iterator structures.  The
 * iterator __begin() and __find() functions will handle all
 * initialization.
 */

struct ngi_unmerged_iter {
	const char *name;
	struct index_state *index;
	/*
	 * cache_entry for stages 1, 2, and 3.
	 * We waste [0] to avoid accidents.
	 */
	struct cache_entry *ce_stages[4];
	int stagemask;

	struct _private {
		int pos[4]; /* only [1,2,3] defined */

		/*
		 * Position in array to look for next item.
		 */
		int pos_next;
	} private;
};

/*
 * Initialize an iterator on the given index to iterate through
 * unmerged entries and search for the first one.
 *
 * Returns 0 if an unmerged entry was found.
 * Returns 1 if not (EOF).
 */
extern int ngi_unmerged_iter__begin(struct ngi_unmerged_iter *iter,
				    struct index_state *index);

/*
 * Get the next unmerged entry using this iterator.
 *
 * Returns 0 if an unmerged entry was found.
 * Returns 1 if not (EOF).
 */
extern int ngi_unmerged_iter__next(struct ngi_unmerged_iter *iter);

/*
 * Find the unmerged entry with the given pathname.  This will
 * initialize the iterator.
 *
 * Returns 0 if an unmerged entry with that pathname was found.
 * Returns 1 if not (EOF).
 */
extern int ngi_unmerged_iter__find(struct ngi_unmerged_iter *iter,
				   struct index_state *index,
				   const char *name);

/*
 * Helper functions for testing.  See t/helper/test-ng-index-api.c
 */
void test__ngi_unmerged_iter(struct index_state *index);

#endif /* NG_INDEX_API_H */
