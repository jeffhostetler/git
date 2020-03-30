#!/bin/sh

test_description='Parallel Checkout (Collisions)'

. ./test-lib.sh

TEST_ROOT="$(pwd)"

write_script <<\EOF "$TEST_ROOT/rot13.sh"
tr \
  'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' \
  'nopqrstuvwxyzabcdefghijklmNOPQRSTUVWXYZABCDEFGHIJKLM'
EOF

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

# This test is based on the collision detection test in t5601.
#
test_expect_success CASE_INSENSITIVE_FS 'clone on case-insensitive fs' '
	test_when_finished "rm -rf warning.* repo.* icasefs" &&

	git init icasefs &&
	git -C icasefs config --local core.parallelcheckoutthreshold 1 &&

	o=$(git -C icasefs hash-object -w --stdin </dev/null | hex2oct) &&
	t=$(printf "100644 File_X\0${o}100644 File_x\0${o}100644 file_X\0${o}100644 file_x\0${o}" |
		git -C icasefs hash-object -w -t tree --stdin) &&
	c=$(git -C icasefs commit-tree -m BranchX $t) &&
	git -C icasefs update-ref refs/heads/BranchX $c &&

	for k in 0 1
	do
		# TODO should we add a GIT_TEST_something to ensure that
		# TODO the parallel-checkout code is actually used?
		#
		# TODO The parallel-checkout version should throw 3 IEC__OPEN
		# TODO errors, since all 4 are parallel-eligible and only 1
		# TODO will win the race.

		GIT_TRACE2_PERF=$TEST_ROOT/trace.$k \
		git -c core.parallelcheckout=$k clone -b BranchX icasefs repo.$k 2>warning.$k &&

		grep File_X warning.$k &&
		grep File_x warning.$k &&
		grep file_X warning.$k &&
		grep file_x warning.$k &&
		test_i18ngrep "the following paths have collided" warning.$k
	done
'

# Files with clean/smudge filters are not eligible for parallel-checkout,
# so they will be processed sequentially *after* all of the parallel-eligible
# files.  Knowing this lets us force a non-random ordering where all of the
# parallel-eligible files are racily created and then the filtered files are
# sequentially created afterwards.
#
# This test steals the rot13 code from t0021.
#
test_expect_success CASE_INSENSITIVE_FS 'clone on case-insensitive fs with filter' '
#	test_when_finished "rm -rf warning.* repo.* icasefs" &&

	git init icasefs &&
	git -C icasefs config --local core.parallelcheckoutthreshold 1 &&

	a=$(echo "File_X filter=rot13" | git -C icasefs hash-object -w --stdin | hex2oct) &&

	o=$(git -C icasefs hash-object -w --stdin </dev/null | hex2oct) &&
	t=$(printf "100644 .gitattributes\0${a}100644 File_X\0${o}100644 File_x\0${o}100644 file_X\0${o}100644 file_x\0${o}" |
		git -C icasefs hash-object -w -t tree --stdin) &&
	c=$(git -C icasefs commit-tree -m BranchX $t) &&
	git -C icasefs update-ref refs/heads/BranchX $c &&

	for k in 0 1
	do
		# TODO should we add a GIT_TEST_something to ensure that
		# TODO the parallel-checkout code is actually used?
		#
		# TODO The parallel-checkout version should throw 2 IEC__OPEN
		# TODO errors, since 3 are parallel-eligible and only 1 will
		# TODO win the race.

		GIT_TRACE2_PERF=$TEST_ROOT/trace.$k \
		git -c core.parallelcheckout=$k \
			-c core.ignoreCase=0 \
			-c filter.rot13.smudge=../rot13.sh \
			-c filter.rot13.clean=../rot13.sh \
			clone -b BranchX icasefs repo.$k 2>warning.$k &&

		grep File_X warning.$k &&
		grep File_x warning.$k &&
		grep file_X warning.$k &&
		grep file_x warning.$k &&
		test_i18ngrep "the following paths have collided" warning.$k
	done
'

test_done
