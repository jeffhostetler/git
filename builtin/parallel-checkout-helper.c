#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "parse-options.h"
#include "pkt-line.h"
#include "trace2.h"

/*
 * This is used as a label prefix in all error messages
 * and in any trace2 messages to identify this child
 * process.
 */
static char t2_child_name[100];
static int t2_child_nr = -1;

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
	if (len < 0 || !line) {
		error("%s: subprocess welcome handshake failed",
		      t2_child_name);
		return 1;
	}
	if (strcmp(line, "parallel-checkout-helper-client")) {
		error("%s: subprocess welcome handshake failed: %s",
		      t2_child_name, line);
		return 1;
	}

	while (1) {
		const char *v;
		len = packet_read_line_gently(0, NULL, &line);
		if (len < 0 || !line)
			break;
		if (!skip_prefix(line, "version=", &v)) {
			error("%s: subprocess version handshake failed: %s",
			      t2_child_name, line);
			return 1;
		}
		b_support_our_version |= (!strcmp(v, OUR_SUBPROCESS_VERSION));
	}
	if (!b_support_our_version) {
		error("%s: client does not support our version: %s",
		      t2_child_name, OUR_SUBPROCESS_VERSION);
		return 1;
	}

	if (packet_write_fmt_gently(1, "parallel-checkout-helper-server\n") ||
	    packet_write_fmt_gently(1, "version=%s\n",
				    OUR_SUBPROCESS_VERSION) ||
	    packet_flush_gently(1)) {
		error("%s: cannot write version handshake", t2_child_name);
		return 1;
	}

	while (1) {
		const char *v;
		int k;

		len = packet_read_line_gently(0, NULL, &line);
		if (len < 0 || !line)
			break;
		if (!skip_prefix(line, "capability=", &v)) {
			error("%s: subprocess capability handshake failed: %s",
			      t2_child_name, line);
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
				error("%s: cannot write capabilities handshake: %s",
				      t2_child_name, caps[k].name);
				return 1;
			}
	if (packet_flush_gently(1)) {
		error("%s: cannot write capabilities handshake", t2_child_name);
		return 1;
	}

	return 0;
}

int cmd_parallel_checkout_helper(int argc, const char **argv,
				 const char *prefix)
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

	if (do_protocol_handshake())
		return 1;

	return err;
}
