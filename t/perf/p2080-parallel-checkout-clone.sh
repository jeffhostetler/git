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

# Classic (sequential) mode for comparison.
#
test_perf 'clone classic' '
	r_out=./r &&

	git -c advice.detachedHead=false \
	    -c core.parallelcheckout=0 \
		clone -- "$GIT_PERF_BORROW_REPO" $r_out &&
	rm -rf $r_out
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
				r_out=./r

				GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
				GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
				git -c core.parallelcheckout=1 \
				    -c core.parallelcheckouthelpers=${h} \
				    -c core.parallelcheckoutwriters=${w} \
				    -c core.parallelcheckoutpreload=${p} \
					clone -- "$GIT_PERF_BORROW_REPO" $r_out &&
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
				r_out=./r

				GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
				GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
				git -c core.parallelcheckout=1 \
				    -c core.parallelcheckouthelpers=${h} \
				    -c core.parallelcheckoutwriters=${w} \
				    -c core.parallelcheckoutpreload=${p} \
					clone -- "$GIT_PERF_BORROW_REPO" $r_out &&
				rm -rf $r_out
			'
		done
	done
done

test_done
