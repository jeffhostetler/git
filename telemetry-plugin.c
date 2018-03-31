#include "cache.h"
#include "telemetry-plugin.h"
#include <dlfcn.h>

/*
 * struct telemetry_plugin is an opaque type out side of this source file.
 * Inside this source file, we map it to a platform-specific type.
 */
struct plugin_dl
{
	void *module_handle;

	int  (*fn_initialize)(void);
	void (*fn_event)(const char *json, int is_final_event);
};

struct telemetry_plugin *telemetry_plugin_load(const char *path)
{
	struct plugin_dl *pi = xcalloc(1, sizeof(struct plugin_dl));
	const char *abs = absolute_path(path);
	const char *error;

	dlerror();
	pi->module_handle = dlopen(abs, RTLD_LAZY | RTLD_LOCAL);
	if (!pi->module_handle) {
		error = dlerror();
		warning("dlopen(%s): %s", abs, error);
		goto fail;
	}

	dlerror();
	pi->fn_initialize = dlsym(pi->module_handle, "plugin_initialize");
	error = dlerror();
	if (error) {
		warning("dlsym(%s, %s): %s", abs, "plugin_initialize", error);
		goto fail;
	}

	dlerror();
	pi->fn_event = dlsym(pi->module_handle, "plugin_event");
	error = dlerror();
	if (error) {
		warning("dlsym(%s, %s): %s", abs, "plugin_event", error);
		goto fail;
	}

	return (struct telemetry_plugin *)pi;

fail:
	if (pi->module_handle)
		dlclose(pi->module_handle);
	free(pi);
	return NULL;
}

void telemetry_plugin_unload(struct telemetry_plugin *tpi)
{
	struct plugin_dl *pi = (struct plugin_dl *)tpi;

	if (pi->module_handle)
		dlclose(pi->module_handle);

	memset(pi, 0, sizeof(struct plugin_dl));
	free(pi);
}

int telemetry_plugin_initialize(struct telemetry_plugin *tpi)
{
	struct plugin_dl *pi = (struct plugin_dl *)tpi;

	return pi && pi->fn_initialize && pi->fn_initialize();
}

void telemetry_plugin_event(struct telemetry_plugin *tpi, const char *json,
			    int is_final_event)
{
	struct plugin_dl *pi = (struct plugin_dl *)tpi;

	if (pi && pi->fn_event)
		return pi->fn_event(json, is_final_event);
}
