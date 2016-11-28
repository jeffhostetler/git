#!/bin/sh

test_description='pre-command hook'

. ./test-lib.sh

test_expect_success 'with no hook' '

	echo "first" > file &&
	git add file &&
	git commit -m "first" &&
	git reset

'

test_expect_success 'now install hook that always succeeds' '
	mkdir -p .git/hooks &&
	write_script .git/hooks/pre-reset <<-EOF
	echo "\$*" >\$(git rev-parse --git-dir)/pre-reset.out
	EOF
'

test_expect_success 'with succeeding hook' '

	echo "second" >> file &&
	git add file &&
	git reset &&
	test mixed = "$(cat .git/pre-reset.out)"

'

test_expect_success 'file with succeeding hook' '

	echo "third" >> file &&
	git add file &&
	git reset HEAD file &&
	test "mixed HEAD file" = "$(cat .git/pre-reset.out)"

'

test_expect_success '--hard with succeeding hook' '

	echo "fourth" >> file &&
	git add file &&
	git reset --hard &&
	test hard = "$(cat .git/pre-reset.out)"

'

test_expect_success 'with failing hook' '

	write_script .git/hooks/pre-reset <<-EOF &&
	exit 1
	EOF
	echo "fifth" >> file &&
	git add file &&
	test_must_fail git reset HEAD file

'

test_done
