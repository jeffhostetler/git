#include "cache.h"
#include "object.h"
#include "object-store.h"
#include "simple-ipc.h"
#include "odb-over-ipc.h"

int odb_over_ipc__is_supported(void)
{
#ifdef SUPPORTS_SIMPLE_IPC
	return 1;
#else
	return 0;
#endif
}

#ifdef SUPPORTS_SIMPLE_IPC
/*
 * We claim "<gitdir>/odb-over-ipc" as the name of the Unix Domain Socket
 * that we will use on Unix.  And something based on this unique string
 * in the Named Pipe File System on Windows.  So we don't need a command
 * line argument for this.
 */
GIT_PATH_FUNC(odb_over_ipc__get_path, "odb-over-ipc");

static int is_daemon = 0;

void odb_over_ipc__set_is_daemon(void)
{
	is_daemon = 1;
}

enum ipc_active_state odb_over_ipc__get_state(void)
{
	return ipc_get_active_state(odb_over_ipc__get_path());
}

// TODO This is a hackathon project, so I'm not going to worry about
// TODO ensuring that full threading works right now.  That is, I'm
// TODO NOT going to give each thread its own connection to the server
// TODO and I'm NOT going to install locking to let concurrent threads
// TODO properly share a single connection.
// TODO
// TODO We already know that the ODB has limited thread-safety, so I'm
// TODO going to rely on our callers to already behave themselves.
//
static struct ipc_client_connection *my_ipc_connection;
static int my_ipc_available = -1;

// TOOD We need someone to call this to close our connection after we
// TODO have finished with the ODB.  Yes, it will be implicitly closed
// TODO when the foreground Git client process exits, but we are
// TODO holding a connection and thread in the `git odb--daemon` open
// TODO and should try to release it quickly.
//
void odb_over_ipc__shutdown_keepalive_connection(void)
{
	if (my_ipc_connection) {
		ipc_client_close_connection(my_ipc_connection);
		my_ipc_connection = NULL;
	}

	/*
	 * Assume that we shutdown a fully functioning connection and
	 * could reconnect again if desired.  Our caller can reset this
	 * assumption, for example when it gets an error.
	 */
	my_ipc_available = 1;
}

int odb_over_ipc__command(const char *command, size_t command_len,
			  struct strbuf *answer)
{
	int ret;

	if (my_ipc_available == -1) {
		enum ipc_active_state state;
		struct ipc_client_connect_options options
			= IPC_CLIENT_CONNECT_OPTIONS_INIT;

		options.wait_if_busy = 1;
		options.wait_if_not_found = 0;

		state = ipc_client_try_connect(odb_over_ipc__get_path(), &options,
					       &my_ipc_connection);
		if (state != IPC_STATE__LISTENING) {
			// error("odb--daemon is not running");
			my_ipc_available = 0;
			return -1;
		}

		my_ipc_available = 1;
	}
	if (!my_ipc_available)
		return -1;

	strbuf_reset(answer);

	ret = ipc_client_send_command_to_connection(my_ipc_connection,
						    command, command_len,
						    answer);

	if (ret == -1) {
		error("could not send '%s' command to odb--daemon", command);
		odb_over_ipc__shutdown_keepalive_connection();
		my_ipc_available = 0;
		return -1;
	}

	return 0;
}

int odb_over_ipc__get_oid(struct repository *r, const struct object_id *oid,
			  struct object_info *oi, unsigned flags)
{
	struct odb_over_ipc__get_oid__request req;

	struct strbuf answer = STRBUF_INIT;
	struct strbuf headers = STRBUF_INIT;
	struct strbuf **lines = NULL;
	const char *sz;
	const char *ch_nul;
	const char *content;
	ssize_t content_len;
	int k;
	int ret;

	if (is_daemon)
		return -1;

	if (r != the_repository)	// TODO not dealing with this
		return -1;

	memset(&req, 0, sizeof(req));
	memcpy(req.key.key, "oid", 4);
	oidcpy(&req.oid, oid);
	req.flags = flags;
	req.want_content = (oi && oi->contentp);

	ret = odb_over_ipc__command((const char *)&req, sizeof(req), &answer);
	if (ret)
		return ret;

	if (!strncmp(answer.buf, "error", 5)) {
		trace2_printf("odb-over-ipc: failed for '%s'", oid_to_hex(oid));
		return -1;
	}

	if (!oi) {
		/*
		 * The caller doesn't care about the object itself;
		 * just whether it exists??
		 */
		goto done;
	}

	/* Find the divider between the headers and the content. */
	ch_nul = strchr(answer.buf, '\0');
	content = ch_nul + 1;
	/* The content_len is only defined if we asked for content. */
	content_len = &answer.buf[answer.len] - content;

	/*
	 * Extract the portion before the divider into another string so that
	 * we can split / parse it by lines.
	 */
	strbuf_add(&headers, answer.buf, (ch_nul - answer.buf));

	lines = strbuf_split_str(headers.buf, '\n', 0);
	strbuf_release(&headers);

	for (k = 0; lines[k]; k++) {
		strbuf_trim_trailing_newline(lines[k]);

		if (skip_prefix(lines[k]->buf, "oid ", &sz)) {
			assert(!strcmp(sz, oid_to_hex(oid)));
			continue;
		}

		if (skip_prefix(lines[k]->buf, "type ", &sz)) {
			enum object_type t = strtol(sz, NULL, 10);
			if (oi->typep)
				*(oi->typep) = t;
			if (oi->type_name)
				strbuf_addstr(oi->type_name, type_name(t));
			continue;
		}

		if (skip_prefix(lines[k]->buf, "size ", &sz)) {
			ssize_t size = strtoumax(sz, NULL, 10);
			if (oi->sizep)
				*(oi->sizep) = size;
			if (oi->contentp && size != content_len)
				BUG("observed content length does not match size");
			continue;
		}

		if (skip_prefix(lines[k]->buf, "disk ", &sz)) {
			if (oi->disk_sizep)
				*(oi->disk_sizep) = strtoumax(sz, NULL, 10);
			continue;
		}

		// TODO do we really care about the delta-base ??
		if (skip_prefix(lines[k]->buf, "delta ", &sz)) {
			if (oi->delta_base_oid) {
				oidclr(oi->delta_base_oid);
				if (get_oid_hex(sz, oi->delta_base_oid)) {
					error("could not parse delta base in odb-over-ipc response");
					ret = -1;
					goto done;
				}
			}
			continue;
		}

		if (skip_prefix(lines[k]->buf, "whence ", &sz)) {
			oi->whence = strtol(sz, NULL, 10);
			continue;
		}

		// TODO The server does not send the contents of oi.u.packed.
		// TODO Do we care?

		BUG("unexpected line '%s' in OID response", lines[k]->buf);
	}

	if (oi->contentp)
		*oi->contentp = xmemdupz(content, content_len);

done:
	if (lines)
		strbuf_list_free(lines);
	strbuf_release(&answer);
	return ret;
}

#endif /* SUPPORTS_SIMPLE_IPC */
