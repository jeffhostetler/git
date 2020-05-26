#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "parse-options.h"

static const char * const helper_usage[] = {
	N_("git checkout-helper [<options>]"),
	NULL
};

int cmd_checkout_helper(int argc, const char **argv, const char *prefix)
{
	int err = 0;

	struct option helper_options[] = {
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(helper_usage, helper_options);

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, helper_options,
			     helper_usage, 0);

	return err;
}
