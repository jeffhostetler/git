#!/bin/sh

test_description='Parallel Checkout'

. ./test-lib.sh

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

	printf "\$Id: 7c8e93673a55d18460dc3ca2866f2236fb20a172 \$\nLINEONE\n"    >A.LF.IDENT.DATA
	printf "\$Id\$\nLINEONE\n"                                               >B.LF.IDENT.DATA
	printf "\$Id: 0000000000000000000000000000000000000000 \$\nLINEONE\n"    >C.LF.IDENT.DATA
	printf "\$Id: ffffffffffffffffffffffffffffffffffffffff \$\nLINEONE\n"    >D.LF.IDENT.DATA
}

setup_eol () {
	git checkout base
	git branch verify_eol_handling
	git checkout verify_eol_handling

	cat >.gitattributes <<-\EOF
		*.dash-text     -text
		*.text-lf       text eol=lf
		*.text-crlf     text eol=crlf
	EOF
	git add .gitattributes
	git commit -m "attrs"

	printf "LINEONE\nLINETWO\nLINETHREE\n"         >LF.DATA
	printf "LINEONE\r\nLINETWO\r\nLINETHREE\r\n" >CRLF.DATA

	cp LF.DATA   f_01.dash-text
	cp LF.DATA   f_02.text-lf
	cp LF.DATA   f_03.text-crlf

	cp CRLF.DATA f_04.dash-text
	cp CRLF.DATA f_05.text-lf
	cp CRLF.DATA f_06.text-crlf

	git add f_*
	git commit -m "setup"
}

confirm_eol_smudging () {
	test_cmp f_01.dash-text LF.DATA
	test_cmp f_02.text-lf   LF.DATA
	test_cmp f_03.text-crlf CRLF.DATA

	test_cmp f_04.dash-text CRLF.DATA
	test_cmp f_05.text-lf   LF.DATA
	test_cmp f_06.text-crlf CRLF.DATA

	cat >expect <<-\EOF
		i/crlf w/crlf attr/-text f_04.dash-text
		i/lf w/crlf attr/text eol=crlf f_03.text-crlf
		i/lf w/crlf attr/text eol=crlf f_06.text-crlf
		i/lf w/lf attr/-text f_01.dash-text
		i/lf w/lf attr/text eol=lf f_02.text-lf
		i/lf w/lf attr/text eol=lf f_05.text-lf
	EOF

	git ls-files f_* --eol |
	sed -e "s/	/ /g" -e "s/  */ /g" |
	sort >actual &&

	test_cmp actual expect
}

setup_encoding () {
	git checkout base
	git branch verify_encoding_handling
	git checkout verify_encoding_handling

	cat >.gitattributes <<-\EOF
		*.text-lf-utf8     text eol=lf working-tree-encoding=utf-8
		*.text-lf-utf16le  text eol=lf working-tree-encoding=utf-16le
		*.text-lf-utf32le  text eol=lf working-tree-encoding=utf-32le
	EOF
	git add .gitattributes
	git commit -m "attrs"

	cp LF.UTF8.DATA    f_01.text-lf-utf8
	cp LF.UTF16LE.DATA f_02.text-lf-utf16le
	cp LF.UTF32LE.DATA f_03.text-lf-utf32le

	git add f_*
	git commit -m "setup"
}

confirm_encoding_smudging () {
	test_cmp f_01.text-lf-utf8    LF.UTF8.DATA
	test_cmp f_02.text-lf-utf16le LF.UTF16LE.DATA
	test_cmp f_03.text-lf-utf32le LF.UTF32LE.DATA

	cat >expect <<-\EOF
		i/lf w/-text attr/text eol=lf f_02.text-lf-utf16le
		i/lf w/-text attr/text eol=lf f_03.text-lf-utf32le
		i/lf w/lf attr/text eol=lf f_01.text-lf-utf8
	EOF

	git ls-files f_* --eol |
	sed -e "s/	/ /g" -e "s/  */ /g" |
	sort >actual

	test_cmp actual expect

	cat >expect <<-\EOF
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

	git check-attr --all f_* | sort >actual

	test_cmp actual expect
}

setup_ident () {
	git checkout base
	git branch verify_ident_handling
	git checkout verify_ident_handling

	cat >.gitattributes <<-\EOF
		*.text-lf-ident    text eol=lf ident
	EOF
	git add .gitattributes
	git commit -m "attrs"

	cp A.LF.IDENT.DATA    f_01.text-lf-ident
	cp B.LF.IDENT.DATA    f_02.text-lf-ident
	cp C.LF.IDENT.DATA    f_03.text-lf-ident
	cp D.LF.IDENT.DATA    f_04.text-lf-ident

	git add f_*
	git commit -m "setup"
}

confirm_ident_smudging () {
	test_cmp f_01.text-lf-ident    A.LF.IDENT.DATA
	test_cmp f_02.text-lf-ident    A.LF.IDENT.DATA
	test_cmp f_03.text-lf-ident    A.LF.IDENT.DATA
	test_cmp f_04.text-lf-ident    A.LF.IDENT.DATA

	cat >expect <<-\EOF
		i/lf w/lf attr/text eol=lf f_01.text-lf-ident
		i/lf w/lf attr/text eol=lf f_02.text-lf-ident
		i/lf w/lf attr/text eol=lf f_03.text-lf-ident
		i/lf w/lf attr/text eol=lf f_04.text-lf-ident
	EOF

	git ls-files f_* --eol |
	sed -e "s/	/ /g" -e "s/  */ /g" |
	sort >actual

	test_cmp actual expect

	cat >expect <<-\EOF
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

	git check-attr --all f_* | sort >actual

	test_cmp actual expect
}

test_expect_success setup '
	setup
'

# Commit a series of files with LF and CRLF and rules to smudge them.
#
# Delete and recreate the files using `reset --hard`.
# This calls `check_updates_loop` to repopulate the files.
#
# Restore them once with the classic sequential logic.
# Then repeat and let `checkout--helper` assist.
#
# The point here is to verify that both give the same answer
# WRT simple file population and basic smudging.
#
test_expect_success verify_eol_handling '
	setup_eol &&

	rm -rf f_* &&
	git -c core.parallelcheckout=0 reset --hard &&
	confirm_eol_smudging &&

	# TODO Consider adding a GIT_TEST_ envvar to output data to let
	# TODO us confirm that the checkout--helper code path was taken
	# TODO when populating these files.

	rm -rf f_* &&
	git -c core.parallelcheckout=1 reset --hard &&
	confirm_eol_smudging
'

test_expect_success verify_encoding_handling '
	setup_encoding &&

	rm -rf f_* &&
	git -c core.parallelcheckout=0 reset --hard &&
	confirm_encoding_smudging &&

	# TODO Consider adding a GIT_TEST_ envvar to output data to let
	# TODO us confirm that the checkout--helper code path was taken
	# TODO when populating these files.

	rm -rf f_* &&
	git -c core.parallelcheckout=1 reset --hard &&
	confirm_encoding_smudging
'

test_expect_success verify_ident_handling '
	setup_ident &&

	rm -rf f_* &&
	git -c core.parallelcheckout=0 reset --hard &&
	confirm_ident_smudging &&

	# TODO Consider adding a GIT_TEST_ envvar to output data to let
	# TODO us confirm that the checkout--helper code path was taken
	# TODO when populating these files.

	rm -rf f_* &&
	git -c core.parallelcheckout=1 reset --hard &&
	confirm_ident_smudging
'

test_done
