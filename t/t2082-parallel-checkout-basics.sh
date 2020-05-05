#!/bin/sh

test_description='Parallel Checkout (Basics)'

. ./test-lib.sh

TEST_ROOT="$(pwd)"

R_BASE=$GIT_BUILD_DIR

# Test to ensure that `parallel-checkout` basically works on a `clone`.
# This confirms that helpers can be started and the resulting worktree
# is populated correctly.
#
# The very verbose settings are very noise, but let us confirm that
# files were created by the helpers.  (So don't bother looking at the
# run times here.)
#
test_expect_success verbose_basic_local_clone '
	test_when_finished "rm -rf *async* *classic* *sync*" &&

	# Use Classic Mode

	GIT_TRACE2_EVENT=$TEST_ROOT/classic.log \
		git -c advice.detachedHead=false \
		    -c core.parallelcheckout=0 \
			clone -- $R_BASE r_classic &&

	# Verify that both the worktree and the index were created correctly.

	git -C r_classic --no-optional-locks \
		status --porcelain=v2 >classic.out &&
	test_must_be_empty classic.out &&
	git -C r_classic diff-files --quiet &&

	# Rebuild the index from scratch, rescan the mtimes, and verify that
	# the worktree was correctly created.
	git -C r_classic read-tree HEAD &&
	git -C r_classic status --porcelain=v2 >classic.out &&
	test_must_be_empty classic.out &&

	# Use Synchronous Mode

	GIT_TRACE2_EVENT=$TEST_ROOT/sync.log \
	  GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
	    GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
	      GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
		git -c advice.detachedHead=false \
		    -c core.parallelcheckout=1 \
		    -c core.parallelcheckouthelpers=2 \
			clone -- $R_BASE r_sync &&

	git -C r_sync --no-optional-locks \
		status --porcelain=v2 >sync.out &&
	test_must_be_empty sync.out &&
	git -C r_sync diff-files --quiet &&

	git -C r_sync read-tree HEAD &&
	git -C r_sync status --porcelain=v2 >sync.out &&
	test_must_be_empty sync.out &&

	# Expect at least one verbose message from the helpers to confirm
	# that we got into the parallel-checkout code.
	grep -q "data_json.*checkout--helper.*writing" sync.log &&
	
	# Confirm that the helpers were started in sync mode.
	grep -q "start.*checkout--helper.*--no-async" sync.log &&

	# Use Asynchronous Mode

	GIT_TRACE2_EVENT=$TEST_ROOT/async.log \
	  GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
	    GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
	      GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
		git -c advice.detachedHead=false \
		    -c core.parallelcheckout=1 \
		    -c core.parallelcheckouthelpers=2 \
			clone -- $R_BASE r_async &&

	git -C r_async --no-optional-locks \
		status --porcelain=v2 >async.out &&
	test_must_be_empty async.out &&
	git -C r_async diff-files --quiet &&

	git -C r_async read-tree HEAD &&
	git -C r_async status --porcelain=v2 >async.out &&
	test_must_be_empty async.out &&

	grep -q "data_json.*checkout--helper.*writing" async.log &&
	grep -q "start.*checkout--helper.*--async" async.log &&

	# Just to be paranoid, actually compare the contents of the
	# worktrees directly.  This avoids any of the caching tricks
	# that Git is doing for us.
	rm -rf r_classic/.git &&
	rm -rf r_sync/.git &&
	rm -rf r_async/.git &&
	diff -r --brief r_classic r_sync &&
	diff -r --brief r_classic r_async
'

test_done
