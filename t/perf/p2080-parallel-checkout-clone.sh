#!/bin/sh

test_description='performance test for parallel-checkout'

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

####test_perf 'clone with sequential checkout' '
####	r_out=./r &&
####
####	git -c advice.detachedHead=false \
####	    -c checkout.workers=1 \
####		clone -- "$GIT_PERF_BORROW_REPO" $r_out &&
####	rm -rf $r_out
####'

# Parallel checkout with one worker per core.
test_perf 'clone with parallel checkout' '
	r_out=./r &&

	git -c advice.detachedHead=false \
	    -c checkout.workers=0 \
		clone -- "$GIT_PERF_BORROW_REPO" $r_out &&
	rm -rf $r_out
'

test_done
