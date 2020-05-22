#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "parallel-checkout-helper.h"
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

/*
 * State machine for processing a blob.
 *
 * New --> Queued --> Loading --> Loaded --> Writing --> Done
 */
enum item_state {
	ITEM_STATE__NEW = 0,
	ITEM_STATE__QUEUED,
	ITEM_STATE__LOADING,
	ITEM_STATE__LOADED,
	ITEM_STATE__WRITING,
	ITEM_STATE__DONE,
};

/*
 * The client sends us the {OID, pathname, and attributes} for each blob
 * that we should populate.  We store that along with the intermediate
 * state and any errors for it in this "item".  This allows a thread to
 * temporarily "own" an item and to delay error reporing back to the
 * foreground process until it is ready for it.
 *
 * The pair `(pc_item_nr, helper_item_nr)` is used to index into arrays
 * in both processes and should always be transmitted/received together.
 */
struct item {
	enum item_state item_state;
	enum parallel_checkout_helper__item_error_class item_error_class;
	int item_errno;

	/* These fields are specified by the client. */
	int pc_item_nr;
	int helper_item_nr;
	struct object_id oid;
	struct conv_attrs ca;
	char *path;
	int mode;

	/* These fields are computed as we load and write the item. */
	int skip;
	int checked_smudge;
	unsigned long content_size;
	void *content;
	struct stat st;
};

static struct item *alloc_item(int pc_item_nr, int helper_item_nr, int mode,
			       int attr, int crlf, int ident,
			       const struct object_id *oid,
			       char *encoding, char *path)
{
	struct item *item = xcalloc(1, sizeof(*item));

	item->item_state = ITEM_STATE__NEW;
	item->item_error_class = IEC__OK;
	item->item_errno = 0;

	item->pc_item_nr = pc_item_nr;
	item->helper_item_nr = helper_item_nr;
	item->mode = mode;
	item->ca.attr_action = attr;
	item->ca.crlf_action = crlf;
	item->ca.ident = ident;
	oidcpy(&item->oid, oid);
	item->ca.working_tree_encoding = encoding;
	item->path = path;

	return item;
}

static void free_item(struct item *item)
{
	if (!item)
		return;

	/*
	 * The `ca.working_tree_encoding` field is defined as `const` and it is
	 * is always assigned a value from the statically initialized table in
	 * convert.c.  However, here in `checkout--helper` we have to cast-away
	 * the const and actually free it, because we allocated it from a field
	 * we received over the wire.
	 */
	free((char *)item->ca.working_tree_encoding);

	free(item->path);
	free(item->content);
	free(item);
}

struct item_vec {
	struct item **array;
	int nr, alloc;
};

static struct item_vec item_vec;

static void free_item_vec(void)
{
	int k;

	for (k = 0; k < item_vec.nr; k++)
		free_item(item_vec.array[k]);

	FREE_AND_NULL(item_vec.array);
	item_vec.nr = 0;
	item_vec.alloc = 0;
}

static void item_vec_append(struct item *item)
{
	/*
	 * As a sanity check, we require the client send us an
	 * helper_item_nr for each item.  This value will later be
	 * used by the client to receive status for the item.  To
	 * avoid building yet another fancy/expensive lookup table, we
	 * require this to be a simple integer matching the item's row
	 * number in our vector.
	 */
	if (item->helper_item_nr != item_vec.nr)
		BUG("invalid helper_item_nr (%d (exp %d)) for '%s'",
		    item->helper_item_nr, item_vec.nr, item->path);

	ALLOC_GROW(item_vec.array, item_vec.nr + 1, item_vec.alloc);
	item_vec.array[item_vec.nr++] = item;

	item->item_state = ITEM_STATE__QUEUED;
}

/*
 * Receive data for an array of items and add them to the item_vec
 * queue.
 *
 * We expect:
 *     command=<cmd>
 *     <binary '__queue_item_record + variant-data' for item k>
 *     <binary '__queue_item_record + variant-data' for item k+1>
 *     <binary '__queue_item_record + variant-data' for item k+2>
 *     ...
 *     <flush>
 *
 * We do not send a response.
 *
 * The client process is free to send one big batch or to send
 * multiple batches of items.
 */
static int helper_cmd__queue(void)
{
	struct item *item = NULL;
	char *data_line;
	int len;
	struct parallel_checkout_helper__queue_item_record fixed_fields;
	char *variant;
	char *encoding;
	char *name;

	while (1) {
		len = packet_read_line_gently(0, NULL, &data_line);
		if (len < 0 || !data_line)
			break;

		if (len < (int)sizeof(fixed_fields))
			BUG("%s[queue]: record too short (obs %d, exp %d)",
			    t2_child_name, len, (int)sizeof(fixed_fields));

		/*
		 * memcpy the fixed portion into a proper structure to
		 * guarantee memory alignment.
		 */
		memcpy(&fixed_fields, data_line, sizeof(fixed_fields));

		variant = data_line + sizeof(fixed_fields);
		if (fixed_fields.len_encoding_name) {
			encoding = xmemdupz(variant,
					    fixed_fields.len_encoding_name);
			variant += fixed_fields.len_encoding_name;
		} else
			encoding = NULL;

		name = xmemdupz(variant, fixed_fields.len_name);

		item = alloc_item(fixed_fields.pc_item_nr,
				  fixed_fields.helper_item_nr,
				  fixed_fields.ce_mode,
				  fixed_fields.attr_action,
				  fixed_fields.crlf_action,
				  fixed_fields.ident,
				  &fixed_fields.oid,
				  encoding,
				  name);
		item_vec_append(item);
	}

	return 0;
}

/*
 * The set of commands/capabilities that this sub-process
 * server advertises and negotiates with the foreground
 * client process.
 */
typedef int (fn_helper_cmd)(void);

struct helper_capability {
	const char *name;
	int client_has;
	fn_helper_cmd *pfn_helper_cmd;
};

static struct helper_capability caps[] = {
	{ "queue",          0, helper_cmd__queue },
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

/*
 * Listen for commands from the client and dispatch them.
 */
static int server_loop(void)
{
	char *line;
	const char *cmd;
	int len;
	int k;

get_next_command:
	len = packet_read_line_gently(0, NULL, &line);
	if (len < 0 || !line)
		return 0;

	if (!skip_prefix(line, "command=", &cmd)) {
		error("%s: invalid sequence '%s'", t2_child_name, line);
		return 1;
	}

	for (k = 0; caps[k].name; k++) {
		if (!strcmp(cmd, caps[k].name)) {
			if (!caps[k].client_has) {
				/*
				 * The client sent a command that it didn't
				 * claim that it understood.
				 */
				error("%s: invalid command '%s'",
				      t2_child_name, line);
				return 1;
			}

			if ((caps[k].pfn_helper_cmd)())
				return 1;

			goto get_next_command;
		}
	}

	/* The server doesn't know about this command. */
	error("%s: unsupported command '%s'", t2_child_name, line);
	return 1;
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

	err = server_loop();

	free_item_vec();

	return err;
}
