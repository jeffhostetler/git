#!/bin/sh

test_description='Parallel Checkout Helper'

. ./test-lib.sh

test_expect_success 'test subprocess handshake' '
	test-tool pkt-line pack >INPUT <<-EOF &&
	parallel-checkout-helper-client
	version=1
	0000
	0000
	EOF

	cat >EXPECT <<-EOF &&
	parallel-checkout-helper-server
	version=1
	0000
	0000
	EOF

	git parallel-checkout-helper <INPUT >OUTPUT &&

	test-tool pkt-line unpack <OUTPUT >ACTUAL &&

	test_cmp EXPECT ACTUAL
'

test_done
