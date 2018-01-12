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

#endif /* NG_INDEX_API_H */
