#!/bin/sh

test_description='Checkout Helper'

. ./test-lib.sh

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

test_done
