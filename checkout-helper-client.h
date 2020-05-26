#ifndef CHECKOUT_HELPER_CLIENT_H
#define CHECKOUT_HELPER_CLIENT_H

/*
 * `checkout_helper_client` (chc) defines the interface used to talk to one
 * or more `checkout-helper` processes.  These processes are long-running
 * and use the sub-process model.
 */

int chc__launch_all_checkout_helpers(int nr_helpers_wanted);
void chc__stop_all_checkout_helpers(void);

#endif /* CHECKOUT_HELPER_CLIENT_H */
