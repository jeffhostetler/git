#!/bin/sh

test_description="Perf test for the builtin FSMonitor"

. ./perf-lib.sh

if ! test_have_prereq FSMONITOR_DAEMON
then
	skip_all="fsmonitor--daemon is not supported on this platform"
	test_done
fi

test_lazy_prereq UNTRACKED_CACHE '
	{ git update-index --test-untracked-cache; ret=$?; } &&
	test $ret -ne 1
'

# Lie to perf-lib and ask for a new empty repo and avoid
# the complaints about GIT_PERF_REPO not being big enough
# the perf hit when GIT_PERF_LARGE_REPO is copied into
# the trash directory.
#
test_perf_fresh_repo

# NEEDSWORK: It would be nice if perf-lib had an option to
# "borrow" an existing large repo (especially for gigantic
# monorepos).  For now, fake it here.
#

# Only used the repeat count for status commands.
# Other commands modify the repo and don't repeat well.
#
COUNT=${GIT_PERF_REPEAT_COUNT:-3}
export COUNT

PARAM_D=4
PARAM_W=10
PARAM_F=9

PARAMS="$PARAM_D"."$PARAM_W"."$PARAM_F"

PARAM_B0=master
export PARAM_B0

PARAM_B1=p0006-ballast
export PARAM_B1

TMP_BR=tmp_br
export TMP_BR

REPO=../repos/gen-many-files-"$PARAMS".git
export REPO

if ! test -d $REPO
then
	(cd ../repos; ./many-files.sh -d $PARAM_D -w $PARAM_W -f $PARAM_F)
else
	# Ensure that FSMonitor is turned off on the borrowed repo.
	#
	git -C $REPO config --unset core.useBuiltinFSMonitor
	git -C $REPO update-index --no-fsmonitor
	test_might_fail git -C $REPO fsmonitor--daemon stop 2>/dev/null

	# Also ensure that it starts in a known state.
	#
	test_might_fail git -C $REPO checkout $PARAM_B0
	test_might_fail git -C $REPO reset --hard
	git -C $REPO clean -d -f
	test_might_fail git -C $REPO branch -D $TMP_BR
fi

echo Data >data.txt

enable_uc() {
	git -C $REPO config core.untrackedcache true
	git -C $REPO update-index --untracked-cache
}

disable_uc() {
	git -C $REPO config core.untrackedcache false
	git -C $REPO update-index --no-untracked-cache
}

start_fsm() {
	git -C $REPO fsmonitor--daemon start
	git -C $REPO fsmonitor--daemon status
	git -C $REPO config core.useBuiltinFSMonitor true
	git -C $REPO update-index --fsmonitor
}

stop_fsm() {
	git -C $REPO config --unset core.useBuiltinFSMonitor 
	git -C $REPO update-index --no-fsmonitor
	test_might_fail git -C $REPO fsmonitor--daemon stop 2>/dev/null
}

# Run status multiple times.  We do this rather than letting
# test_perf() do the looping so that we get data for each
# invocation in the report.
#
# FSMonitor causes subsequent invocations to be much faster.
# This is in addition to any gains on the initial invocation.
#
do_status() {
	msg=$1
	for k in $(test_seq 1 $COUNT); do
		GIT_PERF_REPEAT_COUNT=1 \
			test_perf "$msg[$k]" "
				git -C $REPO status
			"
	done
}

uc_values="false"
test_have_prereq UNTRACKED_CACHE && uc_values="false true"

fsm_values="false true"

for fsm_val in $fsm_values
do
	if test $fsm_val = true
	then
		start_fsm
	fi

	for uc_val in $uc_values
	do
		if test $uc_val = false
		then
			disable_uc
		else
			enable_uc
		fi

		t="[fsm $fsm_val][uc $uc_val]"

		# We begin on the master branch which is almost empty.
		#
		do_status "$t [on $PARAM_B0] status begin"

		# Checkout the ballast branch.
		# This will create thousands of files and directories.
		#
		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $PARAM_B0] checkout $PARAM_B1 as $TMP_BR" "
			git -C $REPO branch $TMP_BR $PARAM_B1 &&
			git -C $REPO checkout $TMP_BR >log 2>&1
		"

		do_status "$t [on $TMP_BR] status after checkout"

		# Modify many files in the temporary branch.
		# Stage them.
		# Commit them.
		# Rollback.
		#
		test_expect_success "$t [on $TMP_BR] modify tracked files" "
			find $REPO -name file1 -exec cp data.txt {} \\;
		"

		do_status "$t [on $TMP_BR] status unstaged"

		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $TMP_BR] add all" "
			git -C $REPO add -A
		"

		do_status "$t [on $TMP_BR] status staged"

		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $TMP_BR] add dot" "
			git -C $REPO add .
		"

		do_status "$t [on $TMP_BR] status staged again"

		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $TMP_BR] commit staged" "
			git -C $REPO commit -a -m data
		"

		do_status "$t [on $TMP_BR] status after commit"

		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $TMP_BR] reset HEAD~1 hard" "
			git -C $REPO reset --hard HEAD~1
		"

		do_status "$t [on $TMP_BR] status after reset"

		# Create some untracked files.
		#
		test_expect_success "$t [on $TMP_BR] create untracked files" "
			cp -R $REPO/ballast/dir1 $REPO/ballast/xxx1
		"

		do_status "$t [on $TMP_BR] status after untracked files"

		# Remove the new untracked files.
		#
		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $TMP_BR] clean -df" "
			git -C $REPO clean -d -f
		"

		do_status "$t [on $TMP_BR] status after clean"

		# Switch back to the master branch.  This will delete
		# the thousands of ballast files and directories.
		#
		GIT_PERF_REPEAT_COUNT=1 \
		test_perf "$t [on $TMP_BR] checkout $PARAM_B0" "
			git -C $REPO checkout $PARAM_B0 >log 2>&1
		"

		do_status "$t [on $PARAM_B0] status end"

		test_expect_success "$t [on $PARAM_B0] delete $TMP_BR" "
			git -C $REPO branch -D $TMP_BR
		"

	done

	if test $fsm_val = true
	then
		stop_fsm
	fi
done

test_done
