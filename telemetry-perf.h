#ifndef TELEMETRY_PERF_H
#define TELEMETRY_PERF_H

/*
 * Helper routines to generate "perf" telemetry events for individual
 * known hot spots in the code.  These are single caller routines
 * that could be static in the various call sites.  They are here for
 * ifdef'ing purposes.
 */

/*
 * read-cache.c:do_read_index()
 */
void telemetry_perf__do_read_index(uint64_t ns_start,
				   const char * path,
				   struct index_state *istate);

/*
 * preload-index.c:preload_index()
 */
void telemetry_perf__preload_index(uint64_t ns_start,
				   int threads, int work,
				   int cache_nr);

/*
 * name-hash.c:lazy_init_name_hash()
 */
void telemetry_perf__lazy_init_name_hash(uint64_t ns_start,
					 struct index_state *istate);

/*
 * wt-status.c:wt_status_collect_untracked()
 */
struct dir_struct;
void telemetry_perf__wt_status_collect_untracked(uint64_t ns_start,
						 const struct dir_struct *dir);

#endif /* TELEMETRY_PERF_H */
