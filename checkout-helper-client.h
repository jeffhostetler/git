#ifndef CHECKOUT_HELPER_CLIENT_H
#define CHECKOUT_HELPER_CLIENT_H

/*
 * `checkout_helper_client` (chc) defines the interface used to talk to one
 * or more `checkout-helper` processes.  These processes are long-running
 * and use the sub-process model.
 */

struct chc_data;
struct chc_item;

int chc__get_value__is_enabled(void);
int chc__get_value__threshold(void);
int chc__get_value__helpers_wanted(void);

int chc__launch_all_checkout_helpers(int nr_helpers_wanted);
void chc__stop_all_checkout_helpers(void);

void chc__free_data(struct chc_data *chc_data);
struct chc_data *chc__alloc_data(const char *base_dir, int base_dir_len);

#endif /* CHECKOUT_HELPER_CLIENT_H */
