#!/bin/sh

test_description="Test internal filesystem monitor"

. ./perf-lib.sh

#
# Performance test for the fsmonitor feature which enables git to talk to a
# file system change monitor and avoid having to scan the working directory
# for new or modified files.
#
# This test uses the builtin ":internal:" fsmonitor feature.  This test was
# adapted from "p7519" which assumes Watchman.
#
# The performance test will also use the untracked cache feature if it is
# available as fsmonitor uses it to speed up scanning for untracked files.
#
# There are 2 environment variables that can be used to alter the default
# behavior of the performance test:
#
# GIT_PERF_7527_UNTRACKED_CACHE: used to configure core.untrackedCache
# GIT_PERF_7527_SPLIT_INDEX: used to configure core.splitIndex
#
# The big win for using fsmonitor is the elimination of the need to scan the
# working directory looking for changed and untracked files. If the file
# information is all cached in RAM, the benefits are reduced.
#
# GIT_PERF_7527_DROP_CACHE: if set, the OS caches are dropped between tests
#

git fsmonitor--daemon --is-supported || {
	skip_all="The internal FSMonitor is not supported on this platform"
	test_done
}

test_perf_large_repo
test_checkout_worktree

test_lazy_prereq UNTRACKED_CACHE '
	{ git update-index --test-untracked-cache; ret=$?; } &&
	test $ret -ne 1
'

GIT_WORK_TREE="$PWD"

if test -n "$GIT_PERF_7527_DROP_CACHE"
then
	# When using GIT_PERF_7527_DROP_CACHE, GIT_PERF_REPEAT_COUNT must be 1 to
	# generate valid results. Otherwise the caching that happens for the nth
	# run will negate the validity of the comparisons.
	if test "$GIT_PERF_REPEAT_COUNT" -ne 1
	then
		echo "warning: Setting GIT_PERF_REPEAT_COUNT=1" >&2
		GIT_PERF_REPEAT_COUNT=1
	fi
fi

test_expect_success "setup for fsmonitor" '
	# set untrackedCache depending on the environment
	if test -n "$GIT_PERF_7527_UNTRACKED_CACHE"
	then
		git config core.untrackedCache "$GIT_PERF_7527_UNTRACKED_CACHE"
	else
		if test_have_prereq UNTRACKED_CACHE
		then
			git config core.untrackedCache true
		else
			git config core.untrackedCache false
		fi
	fi &&

	# set core.splitindex depending on the environment
	if test -n "$GIT_PERF_7527_SPLIT_INDEX"
	then
		git config core.splitIndex "$GIT_PERF_7527_SPLIT_INDEX"
	fi &&

	INTEGRATION_SCRIPT=":internal:" &&

	git config core.fsmonitor "$INTEGRATION_SCRIPT" &&

	# This implicitly starts the fsmonitor daemon.
	git update-index --fsmonitor
'

if test -n "$GIT_PERF_7527_DROP_CACHE"; then
	test-tool drop-caches
fi

test_perf "status (fsmonitor=$INTEGRATION_SCRIPT)" '
	git status
'

if test -n "$GIT_PERF_7527_DROP_CACHE"; then
	test-tool drop-caches
fi

test_perf "status -uno (fsmonitor=$INTEGRATION_SCRIPT)" '
	git status -uno
'

if test -n "$GIT_PERF_7527_DROP_CACHE"; then
	test-tool drop-caches
fi

test_perf "status -uall (fsmonitor=$INTEGRATION_SCRIPT)" '
	git status -uall
'

test_expect_success "setup without fsmonitor" '
	git fsmonitor--daemon --stop &&
	unset INTEGRATION_SCRIPT &&
	git config --unset core.fsmonitor &&
	git update-index --no-fsmonitor
'

if test -n "$GIT_PERF_7527_DROP_CACHE"; then
	test-tool drop-caches
fi

test_perf "status (fsmonitor=$INTEGRATION_SCRIPT)" '
	git status
'

if test -n "$GIT_PERF_7527_DROP_CACHE"; then
	test-tool drop-caches
fi

test_perf "status -uno (fsmonitor=$INTEGRATION_SCRIPT)" '
	git status -uno
'

if test -n "$GIT_PERF_7527_DROP_CACHE"; then
	test-tool drop-caches
fi

test_perf "status -uall (fsmonitor=$INTEGRATION_SCRIPT)" '
	git status -uall
'

git fsmonitor--daemon --stop

test_done
