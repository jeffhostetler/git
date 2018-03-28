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

#endif /* TELEMETRY_H */
