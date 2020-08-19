#!/bin/sh

test_description='performance test for parallel sparse checkout'

. ./perf-lib.sh

# We DO NOT want the test harness to do a clone into our trash
# directory (because we are testing on very large repos and do not
# want to wait for it and because we want our test to measure the
# actual clone time within our test).  So we use the new "borrow"
# feature.  That is, we DO NOT have a read-write repo in the root of
# the trash directory.  The borrowed repo must be considered as
# READ-ONLY.
#
# Our tests here must create <trash_directory>/<repo_name> repos
# for testing purposes.
#
# Within this test script, the path to the borrowed source repo will
# be in $GIT_PERF_BORROW_REPO.
#
# The user should set $GIT_PERF_LARGE_REPO as usual before running
# this test script.
#
test_perf_borrow_large_repo

# In each test we create a nornal local clone (with checkout) of
# the borrowed repo.  The parallel vs sequential checkout is what we
# are interested in timing.
#
# TODO However, in order to get `./run` to work, we also need to delete
# TODO the newly created repo inside the `test_perf` block, so that time
# TODO also gets added to the test.  Is there a way to get around that
# TODO and move the cleanup outside of the timer?

# This test will do a sparse clone (which only contains root-level
# files from the repo).  The user can set $GIT_PERF_SPARSE_SET to the
# pathname of a list of directories to be added to the sparse checkout
# in a second step.  Finally, sparse clone is disabled which causes the
# full repo to be populated.

workers='1 0'

for w in ${workers}
do
	export w
 
	test_perf "sparse clone with w=$w" '
		r_out=./r &&

		git -c advice.detachedHead=false \
		    -c checkout.workers=${w} \
			clone --sparse -- "$GIT_PERF_BORROW_REPO" $r_out &&

		git -C $r_out config --worktree core.sparseCheckoutCone true &&

		if test -n "$GIT_PERF_SPARSE_SET" -a -e "$GIT_PERF_SPARSE_SET" ; then
			cat $GIT_PERF_SPARSE_SET | \
				git -C $r_out \
				    -c checkout.workers=${w} \
					sparse-checkout add --stdin
		fi &&

		git -C $r_out \
		    -c checkout.workers=${w} \
			sparse-checkout disable &&

		git -C $r_out ls-files | wc -l >&2 &&

		rm -rf $r_out
	'
done

test_done
