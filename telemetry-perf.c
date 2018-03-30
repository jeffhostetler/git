#include "cache.h"

void telemetry_perf__do_read_index(uint64_t ns_start,
				   const char * path,
				   struct index_state *istate)
{
	struct json_writer jw = JSON_WRITER_INIT;

	if (!telemetry_perf_want(TELEMETRY_PERF__INDEX))
		return;

	jw_object_begin(&jw, 0);
	{
		jw_object_string(&jw, "path", path);
		jw_object_intmax(&jw, "cache-nr", istate->cache_nr);
	}
	jw_end(&jw);

	telemetry_perf_event(ns_start, TELEMETRY_PERF__INDEX, "do_read_index",
			     &jw);

	jw_release(&jw);
}

void telemetry_perf__preload_index(uint64_t ns_start,
				   int threads, int work,
				   int cache_nr)
{
	struct json_writer jw = JSON_WRITER_INIT;

	if (!telemetry_perf_want(TELEMETRY_PERF__INDEX))
		return;

	jw_object_begin(&jw, 0);
	{
		jw_object_intmax(&jw, "threads", threads);
		jw_object_intmax(&jw, "work", work);
		jw_object_intmax(&jw, "cache-nr", cache_nr);
	}
	jw_end(&jw);

	telemetry_perf_event(ns_start, TELEMETRY_PERF__INDEX, "preload_index",
			     &jw);
	jw_release(&jw);
}

void telemetry_perf__lazy_init_name_hash(uint64_t ns_start,
					 struct index_state *istate)
{
	struct json_writer jw = JSON_WRITER_INIT;
					 
	if (!telemetry_perf_want(TELEMETRY_PERF__INDEX))
		return;

	jw_object_begin(&jw, 0);
	{
		jw_object_intmax(&jw, "cache-nr", istate->cache_nr);
		jw_object_inline_begin_object(&jw, "dir");
		{
			jw_object_intmax(&jw, "count",
					 hashmap_get_size(&istate->dir_hash));
			jw_object_intmax(&jw, "tablesize",
					 istate->dir_hash.tablesize);
		}
		jw_end(&jw);
		jw_object_inline_begin_object(&jw, "name");
		{
			jw_object_intmax(&jw, "count",
					 hashmap_get_size(&istate->name_hash));
			jw_object_intmax(&jw, "tablesize",
					 istate->name_hash.tablesize);
		}
		jw_end(&jw);
	}
	jw_end(&jw);

	telemetry_perf_event(ns_start, TELEMETRY_PERF__INDEX,
			     "lazy_init_name_hash", &jw);
	jw_release(&jw);
}
