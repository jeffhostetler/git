#!/bin/sh

test_description='performance test for sparse-checkout using parallel-checkout'

. ./perf-lib.sh

# The path to the borrowed source repo will be in $GIT_PERF_BORROW_REPO
# We must treat it as read-only.
#
test_perf_borrow_large_repo

if test -z "$GIT_PERF_SPARSE_SET"; then
	echo "warning: \$GIT_PERF_SPARSE_SET is not set, using sparse-checkout disable." >&2
else
	export GIT_PERF_SPARSE_SET
fi

# We create an instance of the repository in each `test_perf`, but
# to allow `./run` to work, we need to delete them inside the
# `test_perf` block, so that time gets added to the test.
#
# TODO Is there a way to get around that?

# Classic (sequential) mode for comparison.
#
test_perf 'sparse-checkout classic' '
	git -c advice.detachedHead=false \
	    -c core.parallelcheckout=0 \
		clone --sparse -- "$GIT_PERF_BORROW_REPO" r_classic &&

	git -C r_classic config --worktree core.sparseCheckoutCone true &&

	if test -z "$GIT_PERF_SPARSE_SET"; then
		git -C r_classic \
		    -c core.parallelcheckout=0 \
			sparse-checkout disable | cat
	else
		cat $GIT_PERF_SPARSE_SET | \
			git -C r_classic \
			    -c core.parallelcheckout=0 \
				sparse-checkout add --stdin | cat
	fi

	git -C r_classic ls-files | wc -l >&2 &&

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

			test_perf "sparse-checkout..sync h${h} w${w} p$p" '
				r_out=./r_sync_h${h}_w${w}_p$p

				GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
				GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
				git -c core.parallelcheckout=1 \
				    -c core.parallelcheckouthelpers=${h} \
				    -c core.parallelcheckoutwriters=${w} \
				    -c core.parallelcheckoutpreload=${p} \
					clone --sparse -- "$GIT_PERF_BORROW_REPO" $r_out &&

				git -C $r_out config --worktree core.sparseCheckoutCone true &&

				if test -z "$GIT_PERF_SPARSE_SET"; then
					GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
					GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
					git -C $r_out \
					    -c core.parallelcheckout=1 \
					    -c core.parallelcheckouthelpers=${h} \
					    -c core.parallelcheckoutwriters=${w} \
					    -c core.parallelcheckoutpreload=${p} \
						sparse-checkout disable | cat
				else
					cat $GIT_PERF_SPARSE_SET | \
						GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
						GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
						git -C $r_out \
						    -c core.parallelcheckout=1 \
						    -c core.parallelcheckouthelpers=${h} \
						    -c core.parallelcheckoutwriters=${w} \
						    -c core.parallelcheckoutpreload=${p} \
							sparse-checkout add --stdin | cat
				fi &&

				git -C $r_out ls-files | wc -l >&2 &&

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

			test_perf "sparse-checkout.async h${h} w${w} p$p" '
				r_out=./r_async_h${h}_w${w}_p$p

				GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
				GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
				git -c core.parallelcheckout=1 \
				    -c core.parallelcheckouthelpers=${h} \
				    -c core.parallelcheckoutwriters=${w} \
				    -c core.parallelcheckoutpreload=${p} \
					clone --sparse -- "$GIT_PERF_BORROW_REPO" $r_out &&

				git -C $r_out config --worktree core.sparseCheckoutCone true &&

				if test -z "$GIT_PERF_SPARSE_SET"; then
					GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
					GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
					git -C $r_out \
					    -c core.parallelcheckout=1 \
					    -c core.parallelcheckouthelpers=${h} \
					    -c core.parallelcheckoutwriters=${w} \
					    -c core.parallelcheckoutpreload=${p} \
						sparse-checkout disable | cat
				else
					cat $GIT_PERF_SPARSE_SET | \
						GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
						GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
						git -C $r_out \
						    -c core.parallelcheckout=1 \
						    -c core.parallelcheckouthelpers=${h} \
						    -c core.parallelcheckoutwriters=${w} \
						    -c core.parallelcheckoutpreload=${p} \
							sparse-checkout add --stdin | cat
				fi &&

				git -C $r_out ls-files | wc -l >&2 &&

				rm -rf $r_out
			'
		done
	done
done

test_done
