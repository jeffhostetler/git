#include "cache.h"
#include "fsmonitor.h"
#include <sys/inotify.h>
#include "khash.h"

KHASH_INIT(path2wd, const char *, int, 1, kh_str_hash_func, kh_str_hash_equal);
#define kh_int_hash_func(ey) key
#define kh_int_hash_equal(a, b) ((a) == (b))
KHASH_INIT(wd2path, int, const char *, 1, kh_int_hash_func, kh_int_hash_equal);

struct fsmonitor_daemon_backend_data {
	int fd_inotify;
	int fd_send_shutdown;
	int fd_wait_shutdown;

	kh_path2wd_t *path2wd;
	kh_wd2path_t *wd2path;
};

static int set_fd_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, NULL);

	if (flags == -1 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		return -1;

	return 0;
}

static int watch_directory(struct fsmonitor_daemon_state *state,
			   const char *path)
{
	uint32_t mask = IN_CLOSE_WRITE | IN_CREATE |
		IN_DELETE | IN_DELETE_SELF | IN_MODIFY |
		IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;
	struct fsmonitor_daemon_backend_data *data = state->backend_data;
	int wd = inotify_add_watch(data->fd_inotify, *path ? path : ".", mask);
	int ret1, ret2;
	khint_t pos1, pos2;

	if (wd < 0)
		return error_errno(_("could not watch '%s'"), path);

	pos1 = kh_put_path2wd(data->path2wd, path, &ret1);
	pos2 = kh_put_wd2path(data->wd2path, wd, &ret2);

	if (pos1 < kh_end(data->path2wd) &&
		pos2 < kh_end(data->wd2path)) {
		path = xstrdup(path);
		kh_key(data->path2wd, pos1) = path;
		kh_value(data->path2wd, pos1) = wd;
		kh_key(data->wd2path, pos2) = wd;
		kh_value(data->wd2path, pos2) = path;
	} else {
		if (pos1)
			kh_del_path2wd(data->path2wd, pos1);
		if (pos2)
			kh_del_wd2path(data->wd2path, pos2);
		inotify_rm_watch(data->fd_inotify, wd);
		return error(_("could not watch '%s"), path);
	}

	return wd;
}

static int watch_directory_recursively(struct fsmonitor_daemon_state *state,
				       struct strbuf *path)
{
	DIR *dir;
	struct dirent *de;

	if (!(dir = opendir(path->len ? path->buf : ".")))
		return error_errno(_("could not open directory '%s'"),
				   path->buf);

	if (watch_directory(state, path->buf) < 0) {
		int save_errno = errno;

		closedir(dir);
		errno = save_errno;
		return error_errno(_("failed to watch '%s'"), path->buf);
	}

	/* Do not watch anything inside the .git/ directory */
	if (!strcmp(".git/", path->buf)) {
		closedir(dir);
		return 0;
	}

	while ((de = readdir(dir))) {
		if (de->d_type == DT_DIR) {
			size_t save = path->len;

			if (!strcmp(de->d_name, ".") ||
			    !strcmp(de->d_name, ".."))
				continue;

			strbuf_addstr(path, de->d_name);
			strbuf_complete(path, '/');
			if (watch_directory_recursively(state, path) < 0) {
				closedir(dir);
				return -1;
			}
			strbuf_setlen(path, save);
		}
	}

	closedir(dir);

	return 0;
}

static int unwatch_directory(struct fsmonitor_daemon_state *state,
			     const char *path)
{
	struct fsmonitor_daemon_backend_data *data = state->backend_data;
	khint_t pos = kh_get_path2wd(data->path2wd, path);

	if (pos < kh_end(data->path2wd)) {
		int wd = kh_value(data->path2wd, pos);
		khint_t pos2 = kh_get_wd2path(data->wd2path, wd);

		free((char *)kh_key(data->path2wd, pos));
		kh_del_path2wd(data->path2wd, pos);
		if (pos2 < kh_end(data->wd2path))
			kh_del_wd2path(data->wd2path, pos2);
		return inotify_rm_watch(data->fd_inotify, wd);
	}

	return 0; /* ignore unseen path */
}

int fsmonitor_listen__ctor(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;
	struct strbuf buf = STRBUF_INIT;
	int pair[2];

	data = xcalloc(1, sizeof(*data));
	data->fd_inotify = -1;
	data->fd_send_shutdown = -1;
	data->fd_wait_shutdown = -1;

	state->backend_data = data;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1 ||
	    set_fd_nonblocking(pair[1]) == -1) {
		error_errno(_("could not create socketpair for inotify"));
		goto failed;
	}
	data->fd_send_shutdown = pair[0];
	data->fd_wait_shutdown = pair[1];

	data->fd_inotify = inotify_init();
	if (data->fd_inotify < 0 ||
	    set_fd_nonblocking(data->fd_inotify) == -1) {
		error_errno(_("could not initialize inotify"));
		goto failed;
	}

	data->path2wd = kh_init_path2wd();
	data->wd2path = kh_init_wd2path();

	if (watch_directory_recursively(state, &buf) < 0) {
		error_errno(_("could not watch '.'"));
		goto failed;
	}

	strbuf_release(&buf);
	return 0;

failed:
	/*
	 * TODO Should we free/release the k-hashes?
	 */
	strbuf_release(&buf);
	close(data->fd_send_shutdown);
	close(data->fd_wait_shutdown);
	close(data->fd_inotify);
	return -1;
}

static void release_inotify_data(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data = state->backend_data;

	if (kh_size(data->path2wd) > 0) {
		const char *path;
		int wd;

		kh_foreach(data->path2wd, path, wd, {
			free((char *)path);
			inotify_rm_watch(data->fd_inotify, wd);
		});
	}
	kh_release_path2wd(data->path2wd);
	kh_release_wd2path(data->wd2path);
}

void fsmonitor_listen__dtor(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data;

	if (!state || !state->backend_data)
		return;

	data = state->backend_data;

	release_inotify_data(state);

	close(data->fd_send_shutdown);
	close(data->fd_wait_shutdown);
	close(data->fd_inotify);

	FREE_AND_NULL(state->backend_data);
}

void fsmonitor_listen__stop_async(struct fsmonitor_daemon_state *state)
{
	struct fsmonitor_daemon_backend_data *data = state->backend_data;

	/*
	 * Write a byte to the shutdown socket pair to wake up and notify
	 * the fsmonitor listener thread.
	 *
	 * TODO Technically, if we just close the send-side of the pipe
	 * TODO pair, the listener poll() should wait up.  So we might
	 * TODO not need this.
	 */
	if (write(state->backend_data->fd_send_shutdown, "Q", 1) < 0)
		error_errno("could not send shutdown to fsmonitor");

	close(data->fd_send_shutdown);
}

void fsmonitor_listen__loop(struct fsmonitor_daemon_state *state)
{
	uint32_t deleted = IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM;
	uint32_t dir_created = IN_CREATE | IN_ISDIR;
	uint32_t dir_deleted = IN_DELETE | IN_ISDIR;
	struct strbuf buf = STRBUF_INIT;
	struct fsmonitor_daemon_backend_data *data = state->backend_data;
	struct pollfd pollfd[2];
	int ret;

	trace2_printf("Start watching: '%s' for inotify", get_git_work_tree());

	for (;;) {
		struct fsmonitor_queue_item dummy, *queue = &dummy;
		uint64_t time = getnanotime();
		char b[sizeof(struct inotify_event) + NAME_MAX + 1], *p;
		int i;

		pollfd[0].fd = data->fd_wait_shutdown;
		pollfd[0].events = POLLIN;

		pollfd[1].fd = data->fd_inotify;
		pollfd[1].events = POLLIN;

		if (poll(pollfd, 2, -1) < 0) {
			if (errno = EINTR)
				continue;
			error_errno(_("could not poll for notifications"));
			goto force_error_stop;
		}

		if (pollfd[0].revents & POLLIN) {
			/* shutdown message queued to socketpair */
			goto shutdown_event;
		}

		assert((pollfd[1].revents & POLLIN) != 0);

		ret = read(data->fd_inotify, &b, sizeof(b));
		if (ret < 0) {
			error_errno(_("could not read() inotify fd"));
			goto force_error_stop;
		}

		/* Ensure strictly increasing timestamps */
		pthread_mutex_lock(&state->queue_update_lock);
		if (time <= state->latest_update)
			time = state->latest_update + 1;
		pthread_mutex_unlock(&state->queue_update_lock);

		for (p = b; ret > 0; ) {
			const struct inotify_event *e = (void *)p;
			size_t incr = sizeof(struct inotify_event) + e->len;
			int special;
			khint_t pos;

			p += incr;
			ret -= incr;

			if (!e->len)
				continue;

			pos = kh_get_wd2path(data->wd2path, e->wd);
			if (!pos)
				continue;

			strbuf_reset(&buf);
			strbuf_addf(&buf, "%s%s", kh_value(data->wd2path, pos),
				    e->name);

			if ((e->mask & dir_created) == dir_created) {
				strbuf_complete(&buf, '/');
				if (watch_directory(state, buf.buf) < 0)
					goto force_error_stop;
			}

			if ((e->mask & dir_deleted) == dir_deleted) {
				if (unwatch_directory(state, buf.buf) < 0) {
					error_errno(_("could not unwatch '%s'"),
						    buf.buf);
					goto force_error_stop;
				}
			}

			special = fsmonitor_special_path(
				state, buf.buf, buf.len, e->mask & deleted);

			if (special > 0) {
				/* ignore paths inside of .git/ */
			}
			else if (!special) {
				/* try to queue normal pathname */
				if (fsmonitor_queue_path(state, &queue, buf.buf,
							 buf.len, time) < 0) {
					error("could not queue '%s'; exiting", buf.buf);
					goto force_error_stop;
				}
			} else if (special == FSMONITOR_DAEMON_QUIT) {
				/* .git directory deleted (or renamed away) */
				trace2_data_string(
					"fsmonitor", the_repository, "message",
					".git directory was removed; quitting");
				goto force_shutdown;
			} else {
				BUG("special %d < 0 for '%s'", special, buf.buf);
			}
		}

		/* Only update the queue if it changed */
		if (queue != &dummy) {
			pthread_mutex_lock(&state->queue_update_lock);
			if (state->first)
				state->first->previous = dummy.previous;
			dummy.previous->next = state->first;
			state->first = queue;
			state->latest_update = time;
			pthread_mutex_unlock(&state->queue_update_lock);
		}

		for (i = 0; i < state->cookie_list.nr; i++) {
			fsmonitor_cookie_seen_trigger(state, state->cookie_list.items[i].string);
		}

		string_list_clear(&state->cookie_list, 0);
	}

force_error_stop:
	state->error_code = -1;

force_shutdown:
	/*
	 * Tell the IPC thead pool to stop (which completes the await
	 * in the main thread (which will also signal this thread (if
	 * we are still alive))).
	 */
	ipc_server_stop_async(state->ipc_server_data);

shutdown_event:
	strbuf_release(&buf);
	return;
}

