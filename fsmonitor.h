#ifndef FSMONITOR_H
#define FSMONITOR_H

extern struct trace_key trace_fsmonitor;

/*
 * Read the the fsmonitor index extension and (if configured) restore the
 * CE_FSMONITOR_VALID state.
 */
extern int read_fsmonitor_extension(struct index_state *istate, const void *data, unsigned long sz);

/*
 * Write the CE_FSMONITOR_VALID state into the fsmonitor index extension.
 */
extern void write_fsmonitor_extension(struct strbuf *sb, struct index_state *istate);

/*
 * Add/remove the fsmonitor index extension
 */
extern void add_fsmonitor(struct index_state *istate);
extern void remove_fsmonitor(struct index_state *istate);

/*
 * Add/remove the fsmonitor index extension as necessary based on the current
 * core.fsmonitor setting.
 */
extern void tweak_fsmonitor(struct index_state *istate);

/*
 * Run the configured fsmonitor integration script and clear the
 * CE_FSMONITOR_VALID bit for any files returned as dirty.  Also invalidate
 * any corresponding untracked cache directory structures. Optimized to only
 * run the first time it is called.
 */
extern void refresh_fsmonitor(struct index_state *istate);

/*
 * Set the given cache entries CE_FSMONITOR_VALID bit.
 */
static inline void mark_fsmonitor_valid(struct cache_entry *ce)
{
	if (core_fsmonitor) {
		ce->ce_flags |= CE_FSMONITOR_VALID;
		trace_printf_key(&trace_fsmonitor, "mark_fsmonitor_clean '%s'", ce->name);
	}
}

/*
 * Clear the given cache entries CE_FSMONITOR_VALID bit and invalidate any
 * corresponding untracked cache directory structures.
 */
static inline void mark_fsmonitor_invalid(struct index_state *istate, struct cache_entry *ce)
{
	if (core_fsmonitor) {
		ce->ce_flags &= ~CE_FSMONITOR_VALID;
		untracked_cache_invalidate_path(istate, ce->name);
		trace_printf_key(&trace_fsmonitor, "mark_fsmonitor_invalid '%s'", ce->name);
	}
}

#endif
