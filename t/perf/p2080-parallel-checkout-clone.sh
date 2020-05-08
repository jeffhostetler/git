#!/bin/sh

test_description='performance test for parallel-checkout'

. ./perf-lib.sh

# The path to the borrowed source repo will be in $GIT_PERF_BORROW_REPO
# We must treat it as read-only.
#
test_perf_borrow_large_repo

# We create an instance of the repository in each `test_perf`, but
# to allow `./run` to work, we need to delete them inside the
# `test_perf` block, so that time gets added to the test.
#
# TODO Is there a way to get around that?
#
#
# Each clone command inclues `| cat` to eat the exit code.
# This is avoid long-pathname problems on Windows.  (Every
# large third-party repo I tested seemed to have a few deeply
# nested files that are too long when appended to the trash
# name directory.)
#
# TODO Is there a better way to handle this?  This was a problem
# even when using the `--root` option (which helps, but not
# enough).

# Classic (sequential) mode for comparison.
#
test_perf 'clone classic' '
	git -c advice.detachedHead=false \
	    -c core.parallelcheckout=0 \
		clone -- "$GIT_PERF_BORROW_REPO" r_classic | cat &&
	rm -rf r_classic
'

# Sync mode.
#
# Sync mode always uses exactly 1 writer regardless what we pass in.
#
test_sync_h_attempts='4'
test_sync_w_attempts='1'
test_sync_p_attempts='02 10'

for h in ${test_sync_h_attempts}
do
	export h
	for w in ${test_sync_w_attempts}
	do
		export w
		for p in ${test_sync_p_attempts}
		do
			export p

			test_perf "clone..sync h${h} w${w} p$p" '
				r_out=./r_sync_h${h}_w${w}_p$p

				GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
				GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
				git -c core.parallelcheckout=1 \
				    -c core.parallelcheckouthelpers=${h} \
				    -c core.parallelcheckoutwriters=${w} \
				    -c core.parallelcheckoutpreload=${p} \
					clone -- $GIT_PERF_BORROW_REPO $r_out | cat &&
				rm -rf $r_out
			'
		done
	done
done

# Async mode.
#
test_async_h_attempts='4'
test_async_w_attempts='1 2 3'
test_async_p_attempts='02 10'

for h in ${test_async_h_attempts}
do
	export h
	for w in ${test_async_w_attempts}
	do
		export w
		for p in ${test_async_p_attempts}
		do
			export p

			test_perf "clone.async h${h} w${w} p$p" '
				r_out=./r_async_h${h}_w${w}_p$p

				GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
				GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
				git -c core.parallelcheckout=1 \
				    -c core.parallelcheckouthelpers=${h} \
				    -c core.parallelcheckoutwriters=${w} \
				    -c core.parallelcheckoutpreload=${p} \
					clone -- "$GIT_PERF_BORROW_REPO" $r_out | cat &&
				rm -rf $r_out
			'
		done
	done
done

test_done
