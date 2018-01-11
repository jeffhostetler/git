#!/bin/sh

test_description='virtual work directory tests'

. ./test-lib.sh

reset_repo () {
	rm .git/index &&
	git -c core.virtualworkdir=false reset --hard HEAD &&
	git -c core.virtualworkdir=false clean -fd &&
	>untracked.txt &&
	>dir1/untracked.txt &&
	>dir2/untracked.txt
}

test_expect_success 'setup' '
	mkdir -p .git/hooks/ &&
	cat >.gitignore <<-\EOF &&
		.gitignore
		expect*
		actual*
	EOF
	>file1.txt &&
	>file2.txt &&
	mkdir -p dir1 &&
	>dir1/file1.txt &&
	>dir1/file2.txt &&
	mkdir -p dir2 &&
	>dir2/file1.txt &&
	>dir2/file2.txt &&
	git add . &&
	git commit -m "initial" &&
	git config --local core.virtualworkdir true
'

test_expect_success 'test hook parameters and version' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		if test "$#" -ne 1
		then
			echo "$0: Exactly 1 argument expected" >&2
			exit 2
		fi

		if test "$1" != 1
		then
			echo "$0: Unsupported hook version." >&2
			exit 1
		fi
	EOF
	git status &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		exit 3
	EOF
	test_must_fail git status
'

test_expect_success 'verify status is clean' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir2/file1.txt\0"
	EOF
	rm -f .git/index &&
	git checkout -f &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir2/file1.txt\0"
		printf "dir1/file1.txt\0"
		printf "dir1/file2.txt\0"
	EOF
	git status >actual &&
	cat >expected <<-\EOF &&
		On branch master
		nothing to commit, working tree clean
	EOF
	test_cmp expected actual
'

test_expect_success 'verify skip-worktree bit is set for absolute path' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file1.txt\0"
	EOF
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		H dir1/file1.txt
		S dir1/file2.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'verify skip-worktree bit is cleared for absolute path' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file2.txt\0"
	EOF
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		S dir1/file1.txt
		H dir1/file2.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'verify folder wild cards' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/\0"
	EOF
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		H dir1/file1.txt
		H dir1/file2.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'verify folders not included are ignored' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file1.txt\0"
		printf "dir1/file2.txt\0"
	EOF
	mkdir -p dir1/dir2 &&
	>dir1/a &&
	>dir1/b &&
	>dir1/dir2/a &&
	>dir1/dir2/b &&
	git add . &&
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		H dir1/file1.txt
		H dir1/file2.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'verify including one file doesnt include the rest' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file1.txt\0"
		printf "dir1/file2.txt\0"
		printf "dir1/dir2/a\0"
	EOF
	mkdir -p dir1/dir2 &&
	>dir1/a &&
	>dir1/b &&
	>dir1/dir2/a &&
	>dir1/dir2/b &&
	git add . &&
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		H dir1/dir2/a
		H dir1/file1.txt
		H dir1/file2.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'verify files not listed are ignored by git clean -f -x' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "untracked.txt\0"
		printf "dir1/\0"
	EOF
	mkdir -p dir3 &&
	>dir3/untracked.txt &&
	git clean -f -x &&
	test_path_is_file file1.txt &&
	test_path_is_file file2.txt &&
	test_path_is_missing untracked.txt &&
	test_path_is_dir dir1 &&
	test_path_is_file dir1/file1.txt &&
	test_path_is_file dir1/file2.txt &&
	test_path_is_missing dir1/untracked.txt &&
	test_path_is_file dir2/file1.txt &&
	test_path_is_file dir2/file2.txt &&
	test_path_is_file dir2/untracked.txt &&
	test_path_is_dir dir3 &&
	test_path_is_file dir3/untracked.txt
'

test_expect_success 'verify files not listed are ignored by git clean -f -d -x' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "untracked.txt\0"
		printf "dir1/\0"
		printf "dir3/\0"
	EOF
	mkdir -p dir3 &&
	>dir3/untracked.txt &&
	git clean -f -d -x &&
	test_path_is_file file1.txt &&
	test_path_is_file file2.txt &&
	test_path_is_missing untracked.txt &&
	test_path_is_dir dir1 &&
	test_path_is_file dir1/file1.txt &&
	test_path_is_file dir1/file2.txt &&
	test_path_is_missing dir1/untracked.txt &&
	test_path_is_file dir2/file1.txt &&
	test_path_is_file dir2/file2.txt &&
	test_path_is_file dir2/untracked.txt &&
	test ! -d dir3 &&
	test_path_is_missing dir3/untracked.txt
'

test_expect_success 'verify folder entries include all files' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/\0"
	EOF
	mkdir -p dir1/dir2 &&
	>dir1/a &&
	>dir1/b &&
	>dir1/dir2/a &&
	>dir1/dir2/b &&
	git status -su >actual &&
	cat >expected <<-\EOF &&
		?? dir1/a
		?? dir1/b
		?? dir1/dir2/a
		?? dir1/dir2/b
		?? dir1/untracked.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'verify case insensitivity of virtual work directory entries' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/a\0"
		printf "Dir1/Dir2/a\0"
		printf "DIR2/\0"
	EOF
	mkdir -p dir1/dir2 &&
	>dir1/a &&
	>dir1/b &&
	>dir1/dir2/a &&
	>dir1/dir2/b &&
	git -c core.ignorecase=false status -su >actual &&
	cat >expected <<-\EOF &&
		?? dir1/a
	EOF
	test_cmp expected actual &&
	git -c core.ignorecase=true status -su >actual &&
	cat >expected <<-\EOF &&
		?? dir1/a
		?? dir1/dir2/a
		?? dir2/untracked.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'on file created' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file3.txt\0"
	EOF
	>dir1/file3.txt &&
	git add . &&
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		S dir1/file1.txt
		S dir1/file2.txt
		H dir1/file3.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'on file renamed' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file1.txt\0"
		printf "dir1/file3.txt\0"
	EOF
	mv dir1/file1.txt dir1/file3.txt &&
	git status -su >actual &&
	cat >expected <<-\EOF &&
		 D dir1/file1.txt
		?? dir1/file3.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'on file deleted' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file1.txt\0"
	EOF
	rm dir1/file1.txt &&
	git status -su >actual &&
	cat >expected <<-\EOF &&
		 D dir1/file1.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'on file overwritten' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/file1.txt\0"
	EOF
	echo "overwritten" >dir1/file1.txt &&
	git status -su >actual &&
	cat >expected <<-\EOF &&
		 M dir1/file1.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'on folder created' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/dir1/\0"
	EOF
	mkdir -p dir1/dir1 &&
	git status -su >actual &&
	cat >expected <<-\EOF &&
	EOF
	test_cmp expected actual &&
	git clean -fd &&
	test ! -d "/dir1/dir1"
'

test_expect_success 'on folder renamed' '
	reset_repo &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir3/\0"
		printf "dir1/file1.txt\0"
		printf "dir1/file2.txt\0"
		printf "dir3/file1.txt\0"
		printf "dir3/file2.txt\0"
	EOF
	mv dir1 dir3 &&
	git status -su >actual &&
	cat >expected <<-\EOF &&
		 D dir1/file1.txt
		 D dir1/file2.txt
		?? dir3/file1.txt
		?? dir3/file2.txt
		?? dir3/untracked.txt
	EOF
	test_cmp expected actual
'

test_expect_success 'folder with same prefix as file' '
	reset_repo &&
	>dir1.sln &&
	write_script .git/hooks/virtual-work-dir <<-\EOF &&
		printf "dir1/\0"
		printf "dir1.sln\0"
	EOF
	git add dir1.sln &&
	git ls-files -v >actual &&
	cat >expected <<-\EOF &&
		H dir1.sln
		H dir1/file1.txt
		H dir1/file2.txt
		S dir2/file1.txt
		S dir2/file2.txt
		S file1.txt
		S file2.txt
	EOF
	test_cmp expected actual
'

test_done
