#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "parse-options.h"
#include "trace2.h"

/*
 * This is used as a label prefix in all error messages and in any trace2
 * messages to identify this child process.
 */
static char t2_child_name[100];
static int t2_child_nr = -1;

/*
 * Allow verbose trace2 logging when this environment variable is set.
 * This is primarily for the test suite to let us confirm that checkout-helper
 * actually did the work.
 *
 * These brackets are somewhat arbitrary.
 */
#define TEST_VERBOSE_LEVEL__OFF           0
#define TEST_VERBOSE_LEVEL__ERRORS        1
#define TEST_VERBOSE_LEVEL__VERBOSE       2
#define TEST_VERBOSE_LEVEL__VERY_VERBOSE  3

static int test_verbose = TEST_VERBOSE_LEVEL__OFF;

static void set_test_verbose(void)
{
	const char *value = getenv("GIT_TEST_CHECKOUT_HELPER_VERBOSE");

	if (!trace2_is_enabled())
		return;

	if (value) {
		int ivalue = strtol(value, NULL, 10);
		if (ivalue > 0)
			test_verbose = ivalue;
	}
}

static const char * const helper_usage[] = {
	N_("git checkout-helper [<options>]"),
	NULL
};

int cmd_checkout_helper(int argc, const char **argv, const char *prefix)
{
	int err = 0;

	struct option helper_options[] = {
		OPT_INTEGER(0, "child", &t2_child_nr, N_("child number")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(helper_usage, helper_options);

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, helper_options,
			     helper_usage, 0);

	snprintf(t2_child_name, sizeof(t2_child_name),
		 "helper[%02d]", t2_child_nr);
	packet_trace_identity(t2_child_name);
	set_test_verbose();

	return err;
}
