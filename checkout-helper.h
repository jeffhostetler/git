#ifndef CHECKOUT_HELPER_H
#define CHECKOUT_HELPER_H

/*
 * This file defines data structures exchanged between the
 * `checkout-helper-client` (chc) API in the foreground process and a
 * background `checkout-helper` process (over the packet-line connection).
 */

/*
 * An enum to classify errors encountered by `checkout-helper` (usually)
 * when trying to populate a file in the worktree.
 *
 * Conceptually, this is half of a { <class>, <errno> } tuple.
 *
 * Note that the default/zero value is __NO_RESULT and rather than _OK
 * so that objects allocated by `calloc()` are setup correctly.
 */
enum checkout_helper__item_error_class {
	IEC__NO_RESULT = 0,
	IEC__OK,
};

#endif /* CHECKOUT_HELPER_H */
