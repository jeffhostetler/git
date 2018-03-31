#ifndef TELEMETRY_PLUGIN_H
#define TELEMETRY_PLUGIN_H

struct telemetry_plugin; /* opaque type */

/*
 * Load the shared library.  Returns NULL on error.
 */
struct telemetry_plugin *telemetry_plugin_load(const char *path);

/*
 * Unload the shared library.  The plugin does not get notified.
 */
void telemetry_plugin_unload(struct telemetry_plugin *tpi);

/*
 * Call into the plugin and let it initialize.  It should return 1
 * if telemetry should be enabled.  It should return 0 otherwise.
 *
 * For example, if there are no consumers for the telemetry stream,
 * it should return 0.
 */
int telemetry_plugin_initialize(struct telemetry_plugin *tpi);

/*
 * Emit a telemetry event to the plugin.  This consists of a single JSON
 * buffer of event-specific data.
 *
 * Set is_final_event for the last event (usually the "exit" event).
 * This allows the plugin to include any final auxilliary data in the stream
 * and cleanup.
 */
void telemetry_plugin_event(struct telemetry_plugin *tpi, const char *json,
			    int is_final_event);

#endif /* TELEMETRY_PLUGIN_H */
