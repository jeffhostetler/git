#include <stdio.h>

/*
 * Initialize any private data.
 *
 * Return 1 if we can emit events and have a consumer.
 */
int plugin_initialize(void)
{
	return 1;
}

/*
 * Emit the given json string as an event.
 */
void plugin_event(const char *json, int is_final_event)
{
	fprintf(stderr, "%s\n", json);
	fflush(stderr);
}
