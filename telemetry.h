#ifndef TELEMETRY_H
#define TELEMETRY_H

void telemetry_alias_event(uint64_t ns_start, int pid, const char **argv,
			   int exit_code);
void telemetry_child_event(uint64_t ns_start, int pid, const char **argv,
			   int exit_code);
void telemetry_hook_event(uint64_t ns_start, int pid, const char **argv,
			  int exit_code);

void telemetry_start_event(int argc, const char **argv);
int  telemetry_exit_event(int exit_code);

void telemetry_set_errmsg(const char *prefix, const char *fmt, va_list ap);
void telemetry_set_branch(const char *branch_name);
void telemetry_set_repository(void);


enum telemetry_perf_token {
	TELEMETRY_PERF__ALL              = -1,
	TELEMETRY_PERF__NONE             = 0,

	TELEMETRY_PERF__INDEX            = 1<<0,
};

int telemetry_perf_want(enum telemetry_perf_token token);
void telemetry_perf_event(uint64_t ns_start, enum telemetry_perf_token token,
			  const char *label, const struct json_writer *jw_data);

#endif /* TELEMETRY_H */
