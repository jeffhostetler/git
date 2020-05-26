#include "test-tool.h"
#include "cache.h"
#include "checkout-helper-client.h"
#include "parse-options.h"

int cmd__checkout_helper(int argc, const char **argv)
{
	int nr_helpers = 1;

	const char *usage[] = {
		"test-tool checkout-helper --helpers=<h>",
		NULL
	};

	struct option options[] = {
		OPT_INTEGER(0, "helpers", &nr_helpers, "number of helpers"),
		OPT_END(),
	};

	argc = parse_options(argc, argv, NULL, options, usage, 0);

	if (chc__launch_all_checkout_helpers(nr_helpers))
		die("could not start checkout-helpers");

	chc__stop_all_checkout_helpers();

	return 0;
}
