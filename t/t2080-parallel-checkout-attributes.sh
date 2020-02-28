#!/bin/sh

test_description='Parallel Checkout (Attributes)'

. ./test-lib.sh

TEST_ROOT="$(pwd)"

# Create some raw data files with well-known properties.
# These will not be under version control, but rather be used
# to create test cases later.

setup () {
	git config --local core.parallelcheckoutthreshold 1

	cat >.gitignore <<-\EOF
		*.DATA
		actual
		expect
	EOF

	git add .gitignore
	git commit -m "gitignore"

	git branch base

	printf "LINEONE\nLINETWO\nLINETHREE\n"         >LF.DATA
	printf "LINEONE\r\nLINETWO\r\nLINETHREE\r\n" >CRLF.DATA

	cp LF.DATA                                   LF.UTF8.DATA
	cat LF.DATA | iconv -f UTF-8 -t UTF-16LE >LF.UTF16LE.DATA
	cat LF.DATA | iconv -f UTF-8 -t UTF-32LE >LF.UTF32LE.DATA

	printf "\$Id: 7c8e93673a55d18460dc3ca2866f2236fb20a172 \$\nLINEONE\n" \
	       >A.LF.IDENT.DATA
	printf "\$Id\$\nLINEONE\n"                                            \
	       >B.LF.IDENT.DATA
	printf "\$Id: 0000000000000000000000000000000000000000 \$\nLINEONE\n" \
	       >C.LF.IDENT.DATA
	printf "\$Id: ffffffffffffffffffffffffffffffffffffffff \$\nLINEONE\n" \
	       >D.LF.IDENT.DATA
}

test_expect_success setup '
	setup
'

# The following tests each commit a series of files and rules to
# smudge them.
#
# Delete and recreate the files using `reset --hard`.
# This calls `check_updates_loop` to repopulate the files.
#
# Restore them once with the classic sequential logic.
# Then repeat and let `checkout--helper` assist.
#
# The point here is to verify that both methods give the same answer
# and that the conv_attr data sent to the helper contains sufficient
# information to smudge files properly (and without access to the
# index or attribute stack).
#
# WARNING: I'm using test_cmp_bin() here rather than test_cmp() because
# WARNING: test_cmp() is a wrapper that calls "test-tool cmp" which calls
# WARNING: strbuf_getline() and it ***EATS*** CRLF vs LF differences and
# WARNING: makes most of these tests useless.

test_expect_success verify_eol_handling '
	test_when_finished "rm -f actual* expect* event.log" &&

	git checkout base &&
	git branch verify_eol_handling &&
	git checkout verify_eol_handling &&

	cat >.gitattributes <<-\EOF &&
		*.dash-text     -text
		*.text-lf       text eol=lf
		*.text-crlf     text eol=crlf
	EOF
	git add .gitattributes &&
	git commit -m "attrs" &&

	printf "LINEONE\nLINETWO\nLINETHREE\n"         >LF.DATA &&
	printf "LINEONE\r\nLINETWO\r\nLINETHREE\r\n" >CRLF.DATA &&

	cp LF.DATA   f_01.dash-text &&
	cp LF.DATA   f_02.text-lf &&
	cp LF.DATA   f_03.text-crlf &&

	cp CRLF.DATA f_04.dash-text &&
	cp CRLF.DATA f_05.text-lf &&
	cp CRLF.DATA f_06.text-crlf &&

	cat >expect <<-\EOF &&
		i/crlf w/crlf attr/-text f_04.dash-text
		i/lf w/crlf attr/text eol=crlf f_03.text-crlf
		i/lf w/crlf attr/text eol=crlf f_06.text-crlf
		i/lf w/lf attr/-text f_01.dash-text
		i/lf w/lf attr/text eol=lf f_02.text-lf
		i/lf w/lf attr/text eol=lf f_05.text-lf
	EOF

	git add f_* &&
	git commit -m "setup" &&

	rm -rf f_* &&
	git -c core.parallelcheckout=0 reset --hard &&

	test_cmp_bin f_01.dash-text LF.DATA &&
	test_cmp_bin f_02.text-lf   LF.DATA &&
	test_cmp_bin f_03.text-crlf CRLF.DATA &&
	test_cmp_bin f_04.dash-text CRLF.DATA &&
	test_cmp_bin f_05.text-lf   LF.DATA &&
	test_cmp_bin f_06.text-crlf CRLF.DATA &&

	git ls-files f_* --eol >actual_0 &&
	sed -e "s/	/ /g" -e "s/  */ /g" <actual_0 | sort >actual &&
	test_cmp_bin actual expect &&

	rm -rf f_* &&

	# Force 2 helper processes instead of relying on the builtin
	# calculation which is based upon the value of `online_cpus()`
	# and may disable parallel-checkout on small PR/CI build machines.

	GIT_TRACE2_EVENT=$TEST_ROOT/event.log \
		GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
			git -c core.parallelcheckout=1 \
			    -c core.parallelcheckouthelpers=2 \
				reset --hard &&

	# Verify that checkout--helper was used on all of the files.
	grep -q "checkout--helper.*writing.*f_01" event.log &&
	grep -q "checkout--helper.*writing.*f_02" event.log &&
	grep -q "checkout--helper.*writing.*f_03" event.log &&
	grep -q "checkout--helper.*writing.*f_04" event.log &&
	grep -q "checkout--helper.*writing.*f_05" event.log &&
	grep -q "checkout--helper.*writing.*f_06" event.log &&

	# And that it only smudged the right ones.
	grep -q -v "checkout--helper.*smudge.*f_01" event.log &&
	grep -q -v "checkout--helper.*smudge.*f_02" event.log &&
	grep -q    "checkout--helper.*smudge.*f_03" event.log &&
	grep -q -v "checkout--helper.*smudge.*f_04" event.log &&
	grep -q -v "checkout--helper.*smudge.*f_05" event.log &&
	grep -q    "checkout--helper.*smudge.*f_06" event.log &&

	test_cmp_bin f_01.dash-text LF.DATA &&
	test_cmp_bin f_02.text-lf   LF.DATA &&
	test_cmp_bin f_03.text-crlf CRLF.DATA &&
	test_cmp_bin f_04.dash-text CRLF.DATA &&
	test_cmp_bin f_05.text-lf   LF.DATA &&
	test_cmp_bin f_06.text-crlf CRLF.DATA &&

	git ls-files f_* --eol >actual_raw &&
	sed -e "s/	/ /g" -e "s/  */ /g" <actual_raw | sort >actual &&
	test_cmp_bin actual expect
'

test_expect_success verify_encoding_handling '
	test_when_finished "rm -f actual* expect*" &&

	git checkout base &&
	git branch verify_encoding_handling &&
	git checkout verify_encoding_handling &&

	cat >.gitattributes <<-\EOF &&
		*.text-lf-utf8     text eol=lf working-tree-encoding=utf-8
		*.text-lf-utf16le  text eol=lf working-tree-encoding=utf-16le
		*.text-lf-utf32le  text eol=lf working-tree-encoding=utf-32le
	EOF
	git add .gitattributes &&
	git commit -m "attrs" &&

	cp LF.UTF8.DATA    f_01.text-lf-utf8 &&
	cp LF.UTF16LE.DATA f_02.text-lf-utf16le &&
	cp LF.UTF32LE.DATA f_03.text-lf-utf32le &&

	git add f_* &&
	git commit -m "setup" &&

	cat >expect_eol <<-\EOF &&
		i/lf w/-text attr/text eol=lf f_02.text-lf-utf16le
		i/lf w/-text attr/text eol=lf f_03.text-lf-utf32le
		i/lf w/lf attr/text eol=lf f_01.text-lf-utf8
	EOF

	cat >expect_all <<-\EOF &&
		f_01.text-lf-utf8: eol: lf
		f_01.text-lf-utf8: text: set
		f_01.text-lf-utf8: working-tree-encoding: utf-8
		f_02.text-lf-utf16le: eol: lf
		f_02.text-lf-utf16le: text: set
		f_02.text-lf-utf16le: working-tree-encoding: utf-16le
		f_03.text-lf-utf32le: eol: lf
		f_03.text-lf-utf32le: text: set
		f_03.text-lf-utf32le: working-tree-encoding: utf-32le
	EOF

	rm -rf f_* &&
	git -c core.parallelcheckout=0 reset --hard &&

	test_cmp_bin f_01.text-lf-utf8    LF.UTF8.DATA &&
	test_cmp_bin f_02.text-lf-utf16le LF.UTF16LE.DATA &&
	test_cmp_bin f_03.text-lf-utf32le LF.UTF32LE.DATA &&

	git ls-files f_* --eol >actual_raw &&
	sed -e "s/	/ /g" -e "s/  */ /g" <actual_raw | sort >actual_eol &&
	test_cmp_bin actual_eol expect_eol &&

	git check-attr --all f_* | sort >actual_all &&
	test_cmp_bin actual_all expect_all &&

	rm -rf f_* &&
	git -c core.parallelcheckout=1 reset --hard &&

	test_cmp_bin f_01.text-lf-utf8    LF.UTF8.DATA &&
	test_cmp_bin f_02.text-lf-utf16le LF.UTF16LE.DATA &&
	test_cmp_bin f_03.text-lf-utf32le LF.UTF32LE.DATA &&

	git ls-files f_* --eol >actual_raw &&
	sed -e "s/	/ /g" -e "s/  */ /g" <actual_raw | sort >actual_eol &&
	test_cmp_bin actual_eol expect_eol &&

	git check-attr --all f_* | sort >actual_all &&
	test_cmp_bin actual_all expect_all
'

test_expect_success verify_ident_handling '
	test_when_finished "rm -f actual* expect* event.log" &&

	git checkout base &&
	git branch verify_ident_handling &&
	git checkout verify_ident_handling &&

	cat >.gitattributes <<-\EOF &&
		*.text-lf-ident    text eol=lf ident
	EOF
	git add .gitattributes &&
	git commit -m "attrs" &&

	cp A.LF.IDENT.DATA    f_01.text-lf-ident &&
	cp B.LF.IDENT.DATA    f_02.text-lf-ident &&
	cp C.LF.IDENT.DATA    f_03.text-lf-ident &&
	cp D.LF.IDENT.DATA    f_04.text-lf-ident &&

	git add f_* &&
	git commit -m "setup" &&

	cat >expect_eol <<-\EOF &&
		i/lf w/lf attr/text eol=lf f_01.text-lf-ident
		i/lf w/lf attr/text eol=lf f_02.text-lf-ident
		i/lf w/lf attr/text eol=lf f_03.text-lf-ident
		i/lf w/lf attr/text eol=lf f_04.text-lf-ident
	EOF

	cat >expect_all <<-\EOF &&
		f_01.text-lf-ident: eol: lf
		f_01.text-lf-ident: ident: set
		f_01.text-lf-ident: text: set
		f_02.text-lf-ident: eol: lf
		f_02.text-lf-ident: ident: set
		f_02.text-lf-ident: text: set
		f_03.text-lf-ident: eol: lf
		f_03.text-lf-ident: ident: set
		f_03.text-lf-ident: text: set
		f_04.text-lf-ident: eol: lf
		f_04.text-lf-ident: ident: set
		f_04.text-lf-ident: text: set
	EOF

	rm -rf f_* &&
	git -c core.parallelcheckout=0 reset --hard &&

	test_cmp_bin f_01.text-lf-ident    A.LF.IDENT.DATA &&
	test_cmp_bin f_02.text-lf-ident    A.LF.IDENT.DATA &&
	test_cmp_bin f_03.text-lf-ident    A.LF.IDENT.DATA &&
	test_cmp_bin f_04.text-lf-ident    A.LF.IDENT.DATA &&

	git ls-files f_* --eol >actual_raw &&
	sed -e "s/	/ /g" -e "s/  */ /g" <actual_raw | sort >actual_eol &&
	test_cmp_bin actual_eol expect_eol &&

	git check-attr --all f_* | sort >actual_all &&
	test_cmp_bin actual_all expect_all &&

	rm -rf f_* &&

	GIT_TRACE2_EVENT=$TEST_ROOT/event.log \
		GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
			git -c core.parallelcheckout=1 \
			    -c core.parallelcheckouthelpers=2 \
				reset --hard &&

	grep -q "checkout--helper.*smudge.*f_01" event.log &&
	grep -q "checkout--helper.*smudge.*f_02" event.log &&
	grep -q "checkout--helper.*smudge.*f_03" event.log &&
	grep -q "checkout--helper.*smudge.*f_04" event.log &&

	test_cmp_bin f_01.text-lf-ident    A.LF.IDENT.DATA &&
	test_cmp_bin f_02.text-lf-ident    A.LF.IDENT.DATA &&
	test_cmp_bin f_03.text-lf-ident    A.LF.IDENT.DATA &&
	test_cmp_bin f_04.text-lf-ident    A.LF.IDENT.DATA &&

	git ls-files f_* --eol >actual_raw &&
	sed -e "s/	/ /g" -e "s/  */ /g" <actual_raw | sort >actual_eol &&
	test_cmp_bin actual_eol expect_eol &&

	git check-attr --all f_* | sort >actual_all &&
	test_cmp_bin actual_all expect_all
'

# When there are nested .gitattribute files, the `check_updates_loop` code
# uses `read_blob_data_from_index` to fetch each of the attribute files and
# build the path-relative attribute stack during the cache-entry iteration.
# This complicates traditional multithreaded ODB access.
#
# The point of this test is to ensure that properly crafted conv_attr data
# is computed and sent to the helper.  We want that both the classic and the
# helper version to produce the same result.

test_expect_success verify_nested_attr '
	test_when_finished "rm -f actual* expect* event.log" &&

	git checkout base &&
	git branch verify_nested_attr &&
	git checkout verify_nested_attr &&

	cat >.gitattributes <<-\EOF &&
		*.text    text eol=lf
	EOF
	git add .gitattributes &&
	git commit -m "attrs" &&

	mkdir d1 d1/d2 &&
	cp LF.DATA d1/d2/f_01.text &&
	cp LF.DATA d1/d2/f_02.text &&
	git add d1 &&

	git commit -m "setup" &&

	cat >d1/.gitattributes <<-\EOF &&
		*.text    text eol=crlf
	EOF
	cat >d1/d2/.gitattributes <<-\EOF &&
		*.text    text eol=crlf
	EOF
	git add d1 &&
	git commit -m "attrs2" &&

	# Switch back to the "base" branch to delete all of the files
	# created in the current branch and the switch back to this
	# branch to have them created with the new (attrs2) attributes.

	git checkout base &&
	git -c core.parallelcheckout=0 checkout verify_nested_attr &&

	test_cmp_bin d1/d2/f_01.text CRLF.DATA &&
	test_cmp_bin d1/d2/f_02.text CRLF.DATA &&

	git checkout base &&

	GIT_TRACE2_EVENT=$TEST_ROOT/event.log \
		GIT_TEST_CHECKOUT_HELPER_VERBOSE=99 \
			git -c core.parallelcheckout=1 \
			    -c core.parallelcheckouthelpers=2 \
				checkout verify_nested_attr &&

	grep -q "checkout--helper.*smudge.*f_01" event.log &&
	grep -q "checkout--helper.*smudge.*f_02" event.log &&

	test_cmp_bin d1/d2/f_01.text CRLF.DATA &&
	test_cmp_bin d1/d2/f_02.text CRLF.DATA
'

test_done
