#!/bin/sh

test_description='git rev-list with object filtering'

. ./test-lib.sh

# test the omit-all filter

test_expect_success 'setup' '
	for n in 1 2 3 4 5 ; do \
		echo $n > file.$n ; \
		git add file.$n ; \
		git commit -m "$n" ; \
	done
'

test_expect_success 'omit-all-blobs omitted 5 blobs' '
	git rev-list HEAD --objects --filter-print-manifest --filter-omit-all-blobs >filtered.output &&
	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 5
'

test_expect_success 'omit-all-blobs blob sha match' '
	sed "s/~//" <blobs.omitted | awk "{print \$1;}" | sort >blobs.sha &&

	git rev-list HEAD --objects >normal.output &&
	awk "/file/ {print \$1;}" <normal.output | sort >normal.sha &&
	test_cmp blobs.sha normal.sha
'

test_expect_success 'omit-all-blobs nothing else changed' '
	grep -v "file" <normal.output | sort >normal.other &&

	grep -v "~" <filtered.output | sort >filtered.other &&
	test_cmp filtered.other normal.other
'

# test the size-based filtering.

test_expect_success 'setup_large' '
	for n in 1000 10000 ; do \
		printf "%"$n"s" X > large.$n ; \
		git add large.$n ; \
		git commit -m "$n" ; \
	done
'

test_expect_success 'omit-large-blobs omit 2 blobs' '
	git rev-list HEAD --objects --filter-print-manifest --filter-omit-large-blobs=500 >filtered.output &&
	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 2
'

test_expect_success 'omit-large-blobs blob sha match' '
	sed "s/~//" <blobs.omitted | awk "{print \$1;}" | sort >blobs.sha &&

	git rev-list HEAD --objects >normal.output &&
	awk "/large/ {print \$1;}" <normal.output | sort >normal.sha &&
	test_cmp blobs.sha normal.sha
'

test_expect_success 'omit-large-blobs nothing else changed' '
	grep -v "large" <normal.output | sort >normal.other &&

	grep -v "~" <filtered.output | sort >filtered.other &&
	test_cmp filtered.other normal.other
'

# boundary test around the size parameter.
# filter is strictly less than the value, so size 500 and 1000 should have the
# same results, but 1001 should filter more.

test_expect_success 'omit-large-blobs omit 2 blobs' '
	git rev-list HEAD --objects --filter-print-manifest --filter-omit-large-blobs=1000 >filtered.output &&
	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 2
'

test_expect_success 'omit-large-blobs omit 1 blob' '
	git rev-list HEAD --objects --filter-print-manifest --filter-omit-large-blobs=1001 >filtered.output &&
	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 1
'

test_expect_success 'omit-large-blobs omit 1 blob (1k)' '
	git rev-list HEAD --objects --filter-print-manifest --filter-omit-large-blobs=1k >filtered.output &&
	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 1
'

test_expect_success 'omit-large-blobs omit no blob (1m)' '
	git rev-list HEAD --objects --filter-print-manifest --filter-omit-large-blobs=1m >filtered.output &&
	test_must_fail grep "^~" filtered.output
'

# Test sparse-pattern filtering (using explicit local patterns).
# We use the same disk format as sparse-checkout to specify the
# filtering, but do not require sparse-checkout to be enabled.

test_expect_success 'setup using sparse file' '
	mkdir dir1 &&
	for n in sparse1 sparse2 ; do \
		echo $n > $n ; \
		git add $n ; \
		echo dir1/$n > dir1/$n ; \
		git add dir1/$n ; \
	done &&
	git commit -m "sparse" &&
	echo dir1/ >pattern1 &&
	echo sparse1 >pattern2
'

# pattern1 should only include the 2 dir1/* files.
# and omit the 5 file.*, 2 large.*, and 2 top-level sparse* files.
test_expect_success 'sparse using path pattern1' '
	git rev-list HEAD --objects --filter-print-manifest --filter-use-path=pattern1 >filtered.output &&

	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 9 &&

	grep "dir1/sparse" filtered.output >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# pattern2 should include the sparse1 and dir1/sparse1.
# and omit the 5 file.*, 2 large.*, and the 2 sparse2 files.
test_expect_success 'sparse using path pattern2' '
	git rev-list HEAD --objects --filter-print-manifest --filter-use-path=pattern2 >filtered.output &&

	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 9 &&

	grep "sparse1" filtered.output >blobs.included &&
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

	git rev-list HEAD --objects --filter-print-manifest --filter-use-blob=`cat pattern1.oid` >filtered.output &&

	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 10 &&

	grep "dir1/sparse" filtered.output >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# repeat previous test but use blob-ish expression rather than OID.
test_expect_success 'sparse using blob-ish to get OID for pattern spec' '
	git rev-list HEAD --objects --filter-print-manifest --filter-use-blob=HEAD:pattern1 >filtered.output &&

	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 10 &&

	grep "dir1/sparse" filtered.output >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# pattern2 should include the sparse1 and dir1/sparse1.
# and omit the 5 file.*, 2 large.*, 2 top-level sparse*, and 2 pattern files.
test_expect_success 'sparse using OID for pattern2' '
	git add pattern2 &&
	git commit -m "pattern2" &&

	git rev-list HEAD --objects >normal.output &&
	grep "pattern2" <normal.output | awk "{print \$1;}" >pattern2.oid &&

	git rev-list HEAD --objects --filter-print-manifest --filter-use-blob=`cat pattern2.oid` >filtered.output &&

	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 11 &&

	grep "sparse1" filtered.output >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

# repeat previous test but use blob-ish expression rather than OID.
test_expect_success 'sparse using blob-ish rather than OID for pattern2' '
	git rev-list HEAD --objects --filter-print-manifest --filter-use-blob=HEAD:pattern2 >filtered.output &&

	grep "^~" filtered.output >blobs.omitted &&
	test $(cat blobs.omitted | wc -l) = 11 &&

	grep "sparse1" filtered.output >blobs.included &&
	test $(cat blobs.included | wc -l) = 2
'

test_done
