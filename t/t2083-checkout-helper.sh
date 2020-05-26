#!/bin/sh

test_description='Checkout Helper'

. ./test-lib.sh

TEST_ROOT="$(pwd)"

test_expect_success 'test subprocess handshake' '
	test-tool pkt-line pack >INPUT <<-EOF &&
	checkout-helper-client
	version=1
	0000
	0000
	EOF

	cat >EXPECT <<-EOF &&
	checkout-helper-server
	version=1
	0000
	0000
	EOF

	git checkout-helper <INPUT >OUTPUT &&

	test-tool pkt-line unpack <OUTPUT >ACTUAL &&

	test_cmp EXPECT ACTUAL
'

test_expect_success 'test subprocess handshake using client code' '
	GIT_TRACE2_EVENT=$TEST_ROOT/event.log \
		test-tool checkout-helper --helpers=4 &&

	# Crude test to "parse" Trace2 JSON and confirm that a sub-process
	# was started.
	#
	grep -q "child_start.*child_id\":0.*checkout-helper.*child=0" event.log &&
	grep -q "child_exit.*child_id\":0.*code\":0" event.log
'

test_done
