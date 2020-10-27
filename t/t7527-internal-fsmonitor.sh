#!/bin/sh

test_description='internal file system watcher'

. ./test-lib.sh

git fsmonitor--daemon --is-supported || {
	skip_all="The internal FSMonitor is not supported on this platform"
	test_done
}

kill_repo () {
	r=$1
	git -C $r fsmonitor--daemon --stop >/dev/null 2>/dev/null
	rm -rf $1
	return 0
}

test_expect_success 'explicit daemon start and stop' '
	test_when_finished "kill_repo test_explicit" &&

	git init test_explicit &&

	git -C test_explicit fsmonitor--daemon --start &&
	sleep 1 &&
	git -C test_explicit fsmonitor--daemon --is-running &&
	git -C test_explicit fsmonitor--daemon --stop &&
	test_must_fail git -C test_explicit fsmonitor--daemon --is-running
'

test_expect_success 'implicit daemon start and stop (delete .git)' '
	test_when_finished "kill_repo test_implicit" &&

	git init test_implicit &&
	test_must_fail git -C test_implicit fsmonitor--daemon --is-running &&

	# query will implicitly start the daemon.
	git -C test_implicit fsmonitor--daemon --query 1 0 >actual &&
	nul_to_q <actual >actual.filtered &&
	grep "^[1-9][0-9]*Q/Q$" actual.filtered &&

	git -C test_implicit fsmonitor--daemon --is-running &&

	# deleting the .git directory will implicitly stop the daemon.
	rm -rf test_implicit/.git &&
	sleep 1 &&

	# Create an empty .git directory so that the following Git command
	# will stay relative to the `-C` directory.  Without this, the Git
	# command will (override the requested -C argument) and crawl out
	# to the containing Git source tree.  This would make the test
	# result dependent upon whether we were using fsmonitor on our
	# development worktree.
	mkdir test_implicit/.git &&

	test_must_fail git -C test_implicit fsmonitor--daemon --is-running
'

test_expect_success 'implicit2 daemon start and stop (rename .git)' '
	test_when_finished "kill_repo test_implicit2" &&

	git init test_implicit2 &&
	test_must_fail git -C test_implicit2 fsmonitor--daemon --is-running &&

	# query will implicitly start the daemon.
	git -C test_implicit2 fsmonitor--daemon --query 1 0 >actual &&
	nul_to_q <actual >actual.filtered &&
	grep "^[1-9][0-9]*Q/Q$" actual.filtered &&

	git -C test_implicit2 fsmonitor--daemon --is-running &&

	# renaming the .git directory will implicitly stop the daemon.
	mv test_implicit2/.git test_implicit2/.xxx &&
	sleep 1 &&

	# Create an empty .git directory so that the following Git command
	# will stay relative to the `-C` directory.  Without this, the Git
	# command will (override the requested -C argument) and crawl out
	# to the containing Git source tree.  This would make the test
	# result dependent upon whether we were using fsmonitor on our
	# development worktree.
	mkdir test_implicit2/.git &&

	test_must_fail git -C test_implicit2 fsmonitor--daemon --is-running
'

test_expect_success 'cannot start multiple daemons' '
	test_when_finished "kill_repo test_multiple" &&

	git init test_multiple &&

	git -C test_multiple fsmonitor--daemon --start &&
	sleep 1 &&
	git -C test_multiple fsmonitor--daemon --is-running &&

	test_must_fail git -C test_multiple fsmonitor--daemon --start 2>actual &&
	grep "fsmonitor daemon is already running" actual &&

	git -C test_multiple fsmonitor--daemon --stop &&
	test_must_fail git -C test_multiple fsmonitor--daemon --is-running
'

test_expect_success 'setup' '
	>tracked &&
	>modified &&
	>delete &&
	>rename &&
	mkdir dir1 &&
	>dir1/tracked &&
	>dir1/modified &&
	>dir1/delete &&
	>dir1/rename &&
	mkdir dir2 &&
	>dir2/tracked &&
	>dir2/modified &&
	>dir2/delete &&
	>dir2/rename &&
	mkdir dirtorename &&
	>dirtorename/a &&
	>dirtorename/b &&

	cat >.gitignore <<-\EOF &&
	.gitignore
	expect*
	actual*
	EOF

	git -c core.fsmonitor= add . &&
	test_tick &&
	git -c core.fsmonitor= commit -m initial &&

	git config core.fsmonitor :internal:
'

test_expect_success 'update-index implicitly starts daemon' '
	test_must_fail git fsmonitor--daemon --is-running &&

	git update-index --fsmonitor &&

	git fsmonitor--daemon --is-running &&

	git fsmonitor--daemon --stop &&
	test_must_fail git fsmonitor--daemon --is-running
'

test_expect_success 'status implicitly starts daemon' '
	test_must_fail git fsmonitor--daemon --is-running &&

	git status >actual &&

	git fsmonitor--daemon --is-running &&

	git fsmonitor--daemon --stop &&
	test_must_fail git fsmonitor--daemon --is-running
'

edit_files() {
	echo 1 >modified
	echo 2 >dir1/modified
	echo 3 >dir2/modified
	>dir1/untracked
}

delete_files() {
	rm -f delete
	rm -f dir1/delete
	rm -f dir2/delete
}

create_files() {
	echo 1 >new
	echo 2 >dir1/new
	echo 3 >dir2/new
}

rename_files() {
	mv rename renamed
	mv dir1/rename dir1/renamed
	mv dir2/rename dir2/renamed
}

file_to_directory() {
	rm -f delete
	mkdir delete
	echo 1 >delete/new
}

directory_to_file() {
	rm -rf dir1
	echo 1 >dir1
}

verify_status() {
	git status >actual &&
	GIT_INDEX_FILE=.git/fresh-index git read-tree master &&
	GIT_INDEX_FILE=.git/fresh-index git -c core.fsmonitor= status >expect &&
	test_cmp expect actual &&
	echo HELLO AFTER &&
	cat .git/trace &&
	echo HELLO AFTER
}

# The next few test cases confirm that our fsmonitor daemon sees each type
# of OS filesystem notification that we care about.  At this layer we just
# ensure we are getting the OS notifications and do not try to confirm what
# is reported by `git status`.
#
# We put a `sleep 1` immediately after starting the daemon to ensure that
# it has a chance to start listening before we start creating FS events.
# This is to help the PR/CI test runs to be less random.
#
# We put a `sleep 1` after the file operations we want to confirm to ensure
# that the daemon has a chance to log the events to our trace log.  This
# helps avoid false failures due to slow log buffer flushing by the daemon.
#
# We `reset` and `clean` at the bottom of each test (and before stopping the
# daemon) because these commands might implicitly restart the daemon.
#
# TODO These tests rely on the trace2_data_string() call at
# TODO fsmonitor--daemon.c:360 which should be guarded with a verbose or
# TODO converted to a trace[1] message or guarded with a GIT_TEST_ symbol
# TODO because we don't want every OS notification causing a trace2 event
# TODO and clogging up the telemetry stream....
#
# TODO Same for the message in fsmonitor_macos.c:167.

clean_up_repo_and_stop_daemon () {
	git reset --hard HEAD
	git clean -fd
	git fsmonitor--daemon --stop
	rm -f .git/trace
}

test_expect_success 'edit some files' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	edit_files &&
	sleep 1 &&

	grep :\"dir1/modified\"  .git/trace &&
	grep :\"dir2/modified\"  .git/trace &&
	grep :\"modified\"       .git/trace &&
	grep :\"dir1/untracked\" .git/trace
'

test_expect_success 'create some files' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	create_files &&
	sleep 1 &&

	grep :\"dir1/new\" .git/trace &&
	grep :\"dir2/new\" .git/trace &&
	grep :\"new\"      .git/trace
'

test_expect_success 'delete some files' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	delete_files &&
	sleep 1 &&

	grep :\"dir1/delete\" .git/trace &&
	grep :\"dir2/delete\" .git/trace &&
	grep :\"delete\"      .git/trace
'

test_expect_success 'rename some files' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	rename_files &&
	sleep 1 &&

	grep :\"dir1/rename\"  .git/trace &&
	grep :\"dir2/rename\"  .git/trace &&
	grep :\"rename\"       .git/trace &&
	grep :\"dir1/renamed\" .git/trace &&
	grep :\"dir2/renamed\" .git/trace &&
	grep :\"renamed\"      .git/trace
'

test_expect_success 'rename directory' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	mv dirtorename dirrenamed &&
	sleep 1 &&

	grep :\"dirtorename/*\" .git/trace &&
	grep :\"dirrenamed/*\"  .git/trace
'

test_expect_success 'file changes to directory' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	file_to_directory &&
	sleep 1 &&

	grep :\"delete\"     .git/trace &&
	grep :\"delete/new\" .git/trace
'

test_expect_success 'directory changes to a file' '
	test_when_finished "clean_up_repo_and_stop_daemon" &&

	GIT_TRACE2_EVENT="$PWD/.git/trace" git fsmonitor--daemon --start &&
	sleep 1 &&

	directory_to_file &&
	sleep 1 &&

	grep :\"dir1\" .git/trace
'

test_done
