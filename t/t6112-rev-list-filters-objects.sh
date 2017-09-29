#!/bin/sh

test_description='git rev-list with object filtering'

. ./test-lib.sh

# test the omit-all filter

test_expect_success 'setup' '
	echo "{print \$1}" >print_1.awk &&
	echo "{print \$2}" >print_2.awk &&

	for n in 1 2 3 4 5
	do
		echo $n > file.$n
		git add file.$n
		git commit -m "$n"
	done
'

# Verify the omitted ("~OID") lines match the predicted list of OIDs.
test_expect_success 'omit-all-blobs omitted 5 blobs' '
	git ls-files -s file.1 file.2 file.3 file.4 file.5 \
		| awk -f print_2.awk \
		| sort >expected &&
	git rev-list HEAD --quiet --objects --filter-print-omitted --filter-omit-all-blobs \
		| awk -f print_1.awk \
		| sed "s/~//" >observed &&
	test_cmp observed expected
'

# Verify the complete OID list matches the unfiltered OIDs plus the omitted OIDs.
test_expect_success 'omit-all-blobs nothing else changed' '
	git rev-list HEAD --objects \
		| awk -f print_1.awk \
		| sort >expected &&
	git rev-list HEAD --objects --filter-print-omitted --filter-omit-all-blobs \
		| awk -f print_1.awk \
		| sed "s/~//" \
		| sort >observed &&
	test_cmp observed expected
'

# test the size-based filtering.

test_expect_success 'setup_large' '
	for n in 1000 10000
	do
		printf "%"$n"s" X > large.$n
		git add large.$n
		git commit -m "$n"
	done
'

test_expect_success 'omit-large-blobs omit 2 blobs' '
	git ls-files -s large.1000 large.10000 \
		| awk -f print_2.awk \
		| sort >expected &&
	git rev-list HEAD --quiet --objects --filter-print-omitted --filter-omit-large-blobs=500 \
		| awk -f print_1.awk \
		| sed "s/~//" >observed &&
	test_cmp observed expected
'

test_expect_success 'omit-large-blobs nothing else changed' '
	git rev-list HEAD --objects \
		| awk -f print_1.awk \
		| sort >expected &&
	git rev-list HEAD --objects --filter-print-omitted --filter-omit-large-blobs=500 \
		| awk -f print_1.awk \
		| sed "s/~//" \
		| sort >observed &&
	test_cmp observed expected
'

# boundary test around the size parameter.
# filter is strictly less than the value, so size 500 and 1000 should have the
# same results, but 1001 should filter more.

test_expect_success 'omit-large-blobs omit 2 blobs' '
	git ls-files -s large.1000 large.10000 \
		| awk -f print_2.awk \
		| sort >expected &&
	git rev-list HEAD --quiet --objects --filter-print-omitted --filter-omit-large-blobs=1000 \
		| awk -f print_1.awk \
		| sed "s/~//" >observed &&
	test_cmp observed expected
'

test_expect_success 'omit-large-blobs omit 1 blob' '
	git ls-files -s large.10000 \
		| awk -f print_2.awk \
		| sort >expected &&
	git rev-list HEAD --quiet --objects --filter-print-omitted --filter-omit-large-blobs=1001 \
		| awk -f print_1.awk \
		| sed "s/~//" >observed &&
	test_cmp observed expected
'

test_expect_success 'omit-large-blobs omit 1 blob (1k)' '
	git ls-files -s large.10000 \
		| awk -f print_2.awk \
		| sort >expected &&
	git rev-list HEAD --quiet --objects --filter-print-omitted --filter-omit-large-blobs=1k \
		| awk -f print_1.awk \
		| sed "s/~//" >observed &&
	test_cmp observed expected
'

test_expect_success 'omit-large-blobs omit no blob (1m)' '
	cat </dev/null >expected &&
	git rev-list HEAD --quiet --objects --filter-print-omitted --filter-omit-large-blobs=1m \
		| awk -f print_1.awk \
		| sed "s/~//" >observed &&
	test_cmp observed expected
'

# Test sparse-pattern filtering (using explicit local patterns).
# We use the same disk format as sparse-checkout to specify the
# filtering, but do not require sparse-checkout to be enabled.

test_expect_success 'setup using sparse file' '
	mkdir dir1 &&
	for n in sparse1 sparse2
	do
		echo $n > $n
		git add $n
		echo dir1/$n > dir1/$n
		git add dir1/$n
	done &&
	git commit -m "sparse" &&
	echo dir1/ >pattern1 &&
	echo sparse1 >pattern2
'

# pattern1 should only include the 2 dir1/* files.
# and omit the 5 file.*, 2 large.*, and 2 top-level sparse* files.
test_expect_success 'sparse using path pattern1' '
	git rev-list HEAD --objects --filter-print-omitted --filter-use-path=pattern1 >out &&

	grep "^~" out >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 9 &&

	grep "dir1/sparse" out >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# pattern2 should include the sparse1 and dir1/sparse1.
# and omit the 5 file.*, 2 large.*, and the 2 sparse2 files.
test_expect_success 'sparse using path pattern2' '
	git rev-list HEAD --objects --filter-print-omitted --filter-use-path=pattern2 >out &&

	grep "^~" out >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 9 &&

	grep "sparse1" out >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# Test sparse-pattern filtering (using a blob in the repo).
# This could be used to later let pack-objects do filtering.

# pattern1 should only include the 2 dir1/* files.
# and omit the 5 file.*, 2 large.*, 2 top-level sparse*, and 1 pattern file.
test_expect_success 'sparse using OID for pattern1' '
	git add pattern1 &&
	git commit -m "pattern1" &&

	git rev-list HEAD --objects >normal.output &&
	grep "pattern1" <normal.output | awk "{print \$1;}" >pattern1.oid &&

	git rev-list HEAD --objects --filter-print-omitted --filter-use-blob=`cat pattern1.oid` >out &&

	grep "^~" out >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 10 &&

	grep "dir1/sparse" out >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# repeat previous test but use blob-ish expression rather than OID.
test_expect_success 'sparse using blob-ish to get OID for pattern spec' '
	git rev-list HEAD --objects --filter-print-omitted --filter-use-blob=HEAD:pattern1 >out &&

	grep "^~" out >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 10 &&

	grep "dir1/sparse" out >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# pattern2 should include the sparse1 and dir1/sparse1.
# and omit the 5 file.*, 2 large.*, 2 top-level sparse*, and 2 pattern files.
test_expect_success 'sparse using OID for pattern2' '
	git add pattern2 &&
	git commit -m "pattern2" &&

	git rev-list HEAD --objects >normal.output &&
	grep "pattern2" <normal.output | awk "{print \$1;}" >pattern2.oid &&

	git rev-list HEAD --objects --filter-print-omitted --filter-use-blob=`cat pattern2.oid` >out &&

	grep "^~" out >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 11 &&

	grep "sparse1" out >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# repeat previous test but use blob-ish expression rather than OID.
test_expect_success 'sparse using blob-ish rather than OID for pattern2' '
	git rev-list HEAD --objects --filter-print-omitted --filter-use-blob=HEAD:pattern2 >out &&

	grep "^~" out >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 11 &&

	grep "sparse1" out >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# delete some loose objects and test rev-list printing them as missing.
test_expect_success 'print missing objects' '
	git ls-files -s file.1 file.2 file.3 file.4 file.5 \
		| awk -f print_2.awk \
		| sort >expected &&
	for id in `cat expected | sed "s|..|&/|"`
	do
		rm .git/objects/$id
	done &&
	git rev-list --quiet HEAD --filter-print-missing --objects \
		| awk -f print_1.awk \
		| sed "s/?//" \
		| sort >observed &&
	test_cmp observed expected
'

test_done
