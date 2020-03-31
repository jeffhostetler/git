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
# cache-entries[0, k-1] for the other entry.
#
# This is not sufficient in async mode where parallel-eligible files
# are created in parallel, because colliding files may be created in a
# racy order.
#
# These tests are to verify that the collision detection is extended
# to allow for racy order.
#
# This test is based on the collision detection test in t5601.
#
test_expect_success CASE_INSENSITIVE_FS 'clone on case-insensitive fs' '
	test_when_finished "rm -rf warning.* repo.* event.* icasefs" &&

	git init icasefs &&
	git -C icasefs config --local core.parallelcheckoutthreshold 1 &&

	o=$(git -C icasefs hash-object -w --stdin </dev/null | hex2oct) &&
	t=$(printf "100644 File_X\0${o}100644 File_x\0${o}100644 file_X\0${o}100644 file_x\0${o}" |
		git -C icasefs hash-object -w -t tree --stdin) &&
	c=$(git -C icasefs commit-tree -m BranchX $t) &&
	git -C icasefs update-ref refs/heads/BranchX $c &&

	git -c core.parallelcheckout=0 clone -b BranchX icasefs repo.0 2>warning.0 &&

	grep File_X warning.0 &&
	grep File_x warning.0 &&
	grep file_X warning.0 &&
	grep file_x warning.0 &&
	test_i18ngrep "the following paths have collided" warning.0 &&

	# The sync version should behave exactly the same as the
	# classic version.
	#
	# Force 2 helper processes instead of relying on the builtin
	# calculation which is based upon the value of `online_cpus()`
	# and may disable parallel-checkout on small PR/CI build machines.

	GIT_TRACE2_EVENT=$TEST_ROOT/event.1 \
	  GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
	    GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
	      GIT_TEST_PARALLEL_CHECKOUT_MODE=sync \
		git -c core.parallelcheckout=1 \
		    -c core.parallelcheckouthelpers=2 \
			clone -b BranchX icasefs repo.1 2>warning.1 &&

	# Verify that checkout--helper was used on all of the files.
	grep -q "checkout--helper.*writing.*File_X" event.1 &&
	grep -q "checkout--helper.*writing.*File_x" event.1 &&
	grep -q "checkout--helper.*writing.*file_X" event.1 &&
	grep -q "checkout--helper.*writing.*file_x" event.1 &&

	grep -q -v "checkout--helper.*open_failed" event.1 &&

	grep File_X warning.1 &&
	grep File_x warning.1 &&
	grep file_X warning.1 &&
	grep file_x warning.1 &&
	test_i18ngrep "the following paths have collided" warning.1 &&

	# The async version should behave the similarly but also get 3
	# IEC__OPEN errors, since all 4 files are parallel-eligible
	# and only 1 will win the race (before we fallback to the classic
	# code to fixes things).

	GIT_TRACE2_EVENT=$TEST_ROOT/event.2 \
	  GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
	    GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
	      GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
		git -c core.parallelcheckout=1 \
		    -c core.parallelcheckouthelpers=2 \
			clone -b BranchX icasefs repo.2 2>warning.2 &&

	# Verify that checkout--helper was used on all of the files.
	grep -q "checkout--helper.*writing.*File_X" event.2 &&
	grep -q "checkout--helper.*writing.*File_x" event.2 &&
	grep -q "checkout--helper.*writing.*file_X" event.2 &&
	grep -q "checkout--helper.*writing.*file_x" event.2 &&

	grep "checkout--helper.*open_failed" event.2 >event.failed.2 &&
	test_line_count = 3 event.failed.2 &&

	grep File_X warning.2 &&
	grep File_x warning.2 &&
	grep file_X warning.2 &&
	grep file_x warning.2 &&
	test_i18ngrep "the following paths have collided" warning.2
'

# Files with clean/smudge filters are not parallel-eligible.  They are
# processed sequentially (in the "async/remainder" region) *after* all
# of the parallel-eligible files.
#
# This test uses ".gitattributes" to force an alternate ordering.
# We make the (alphabetically) first file non-eligible to cause it
# to be populated last.
#
# We have to use "core.ignoreCase=0" so that only the first file matches
# the pattern.
#
# However, this test does not work on Windows because collision detection
# is an approximation (using strcasecmp()).  It works on OSX because
# collision detection uses inode numbers for exact matching.
#
# This test steals the rot13 code from t0021.
#
test_expect_success CASE_INSENSITIVE_FS,!MINGW,!CYGWIN 'clone on case-insensitive fs with filter' '
	test_when_finished "rm -rf warning.* repo.* event.* icasefs" &&

	git init icasefs &&
	git -C icasefs config --local core.parallelcheckoutthreshold 1 &&

	a=$(echo "File_X filter=rot13" | git -C icasefs hash-object -w --stdin | hex2oct) &&

	o=$(git -C icasefs hash-object -w --stdin </dev/null | hex2oct) &&
	t=$(printf "100644 .gitattributes\0${a}100644 File_X\0${o}100644 File_x\0${o}100644 file_X\0${o}100644 file_x\0${o}" |
		git -C icasefs hash-object -w -t tree --stdin) &&
	c=$(git -C icasefs commit-tree -m BranchX $t) &&
	git -C icasefs update-ref refs/heads/BranchX $c &&

	git -c core.parallelcheckout=0 \
		-c core.ignoreCase=0 \
		-c filter.rot13.smudge=../rot13.sh \
		-c filter.rot13.clean=../rot13.sh \
		clone -b BranchX icasefs repo.0 2>warning.0 &&

	grep File_X warning.0 &&
	grep File_x warning.0 &&
	grep file_X warning.0 &&
	grep file_x warning.0 &&
	test_i18ngrep "the following paths have collided" warning.0 &&

	GIT_TRACE2_EVENT=$TEST_ROOT/event.2 \
	  GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
	    GIT_TEST_PARALLEL_CHECKOUT_THRESHOLD=1 \
	      GIT_TEST_PARALLEL_CHECKOUT_MODE=async \
		git -c core.parallelcheckout=1 \
		    -c core.parallelcheckouthelpers=2 \
		    -c core.ignoreCase=0 \
		    -c filter.rot13.smudge=../rot13.sh \
		    -c filter.rot13.clean=../rot13.sh \
			clone -b BranchX icasefs repo.2 2>warning.2 &&

	# Verify that checkout--helper was ONLY used on the non-rot13 files.
	grep -q -v "checkout--helper.*writing.*File_X" event.2 &&
	grep -q    "checkout--helper.*writing.*File_x" event.2 &&
	grep -q    "checkout--helper.*writing.*file_X" event.2 &&
	grep -q    "checkout--helper.*writing.*file_x" event.2 &&

	# Verify that we got IEC__OPEN errors for 2 of the 3 files (which
	# also ensures that the non-eligible files are populated after the
	# eligible ones).
	grep "checkout--helper.*open_failed" event.2 >event.failed.2 &&
	test_line_count = 2 event.failed.2 &&

	# Verify that the set of files in the collision matches the
	# classic version.
	grep File_X warning.2 &&
	grep File_x warning.2 &&
	grep file_X warning.2 &&
	grep file_x warning.2 &&
	test_i18ngrep "the following paths have collided" warning.2
'

test_done
