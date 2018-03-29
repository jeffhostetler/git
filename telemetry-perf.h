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

#endif /* TELEMETRY_PERF_H */
