#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "parse-options.h"
#include "pkt-line.h"

static const char * const helper_usage[] = {
	N_("git parallel-checkout-helper [<options>]"),
	NULL
};

typedef int (fn_helper_cmd)(void);

struct helper_capability {
	const char *name;
	int client_has;
	fn_helper_cmd *pfn_helper_cmd;
};

static struct helper_capability caps[] = {
	{ NULL, 0, NULL },
};

/*
 * Handle the subprocess protocol handshake as described in:
 * [] Documentation/technical/protocol-common.txt
 * [] Documentation/technical/long-running-process-protocol.txt
 *
 * Return 1 if we have a protocol error.
 */
static int do_protocol_handshake(void)
{
#define OUR_SUBPROCESS_VERSION "1"

	char *line;
	int len;
	int k;
	int b_support_our_version = 0;

	len = packet_read_line_gently(0, NULL, &line);
	if (len < 0 || !line || strcmp(line, "parallel-checkout-helper-client")) {
		error("server: subprocess welcome handshake failed: %s", line);
		return 1;
	}

	while (1) {
		const char *v;
		len = packet_read_line_gently(0, NULL, &line);
		if (len < 0 || !line)
			break;
		if (!skip_prefix(line, "version=", &v)) {
			error("server: subprocess version handshake failed: %s",
			      line);
			return 1;
		}
		b_support_our_version |= (!strcmp(v, OUR_SUBPROCESS_VERSION));
	}
	if (!b_support_our_version) {
		error("server: client does not support our version: %s",
		      OUR_SUBPROCESS_VERSION);
		return 1;
	}

	if (packet_write_fmt_gently(1, "parallel-checkout-helper-server\n") ||
	    packet_write_fmt_gently(1, "version=%s\n",
				    OUR_SUBPROCESS_VERSION) ||
	    packet_flush_gently(1)) {
		error("server: cannot write version handshake");
		return 1;
	}

	while (1) {
		const char *v;
		int k;

		len = packet_read_line_gently(0, NULL, &line);
		if (len < 0 || !line)
			break;
		if (!skip_prefix(line, "capability=", &v)) {
			error("server: subprocess capability handshake failed: %s",
			      line);
			return 1;
		}
		for (k = 0; caps[k].name; k++)
			if (!strcmp(v, caps[k].name))
				caps[k].client_has = 1;
	}

	for (k = 0; caps[k].name; k++)
		if (caps[k].client_has)
			if (packet_write_fmt_gently(1, "capability=%s\n",
						    caps[k].name)) {
				error("server: cannot write capabilities handshake: %s",
				      caps[k].name);
				return 1;
			}
	if (packet_flush_gently(1)) {
		error("server: cannot write capabilities handshake");
		return 1;
	}

	return 0;
}

int cmd_parallel_checkout_helper(int argc, const char **argv,
				 const char *prefix)
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

	if (do_protocol_handshake())
		return 1;

	return err;
}
