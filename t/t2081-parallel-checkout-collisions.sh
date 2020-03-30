#!/bin/sh

test_description='Parallel Checkout (Collisions)'

. ./test-lib.sh

# When there are pathname collisions during a clone on a case-insensitive
# filesystem, Git should report a warning listing the all of the colliding
# files.  The sequential code does collision detection by calling lstat()
# before trying to open(O_CREAT) the file.  Then for item k, it searches
# cache-entries[0, k-1] for the other entry.  This is not sufficient when
# files are created in parallel, since the colliding file pair may be created
# in a racy order.
#
# These tests are to verify that the collision detection is extended to allow
# for racy order.

test_expect_success CASE_INSENSITIVE_FS 'clone on case-insensitive fs' '
	git init icasefs &&
	git -C icasefs config --local core.parallelcheckoutthreshold 1 &&
	o=$(git -C icasefs hash-object -w --stdin </dev/null | hex2oct) &&
	t=$(printf "100644 File_X\0${o}100644 File_x\0${o}100644 file_X\0${o}100644 file_x\0${o}" |
		git -C icasefs hash-object -w -t tree --stdin) &&
	c=$(git -C icasefs commit-tree -m BranchX $t) &&
	git -C icasefs update-ref refs/heads/BranchX $c &&

	for k in 0 1
	do
		GIT_TRACE2_PERF=$(pwd)/trace.$k git -c core.parallelcheckout=$k clone -b BranchX icasefs repo.$k 2>warning.$k &&

		grep File_X warning.$k &&
		grep File_x warning.$k &&
		grep file_X warning.$k &&
		grep file_x warning.$k &&
		test_i18ngrep "the following paths have collided" warning.$k
	done
'

test_done
