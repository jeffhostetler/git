#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include "git-compat-util.h"
#include <pthread.h>

/*
 * Generic implementation of an unbounded Multi-Producer-Multi-Consumer
 * queue.
 */

/*
 * struct mpmcq_entry is an opaque structure representing an entry in the
 * queue.
 */
struct mpmcq_entry {
	struct mpmcq_entry *next;
};

/*
 * struct mpmcq is the concurrent queue structure. Members should not be
 * modified directly.
 */
struct mpmcq {
	struct mpmcq_entry *head;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	int cancel;
};

/*
 * Initializes a mpmcq_entry structure.
 *
 * `entry` points to the entry to initialize.
 *
 * The mpmcq_entry structure does not hold references to external resources,
 * and it is safe to just discard it once you are done with it (i.e. if
 * your structure was allocated with xmalloc(), you can just free() it,
 * and if it is on stack, you can just let it go out of scope).
 */
static inline void mpmcq_entry_init(struct mpmcq_entry *entry)
{
	entry->next = NULL;
}

/*
 * Initializes a mpmcq structure.
 */
extern void mpmcq_init(struct mpmcq *queue);

/*
 * Destroys a mpmcq structure.
 */
extern void mpmcq_destroy(struct mpmcq *queue);

/*
 * Pushes an entry on to the queue.
 *
 * `queue` is the mpmcq structure.
 * `entry` is the entry to push.
 */
extern void mpmcq_push(struct mpmcq *queue, struct mpmcq_entry *entry);

/*
 * Pops an entry off the queue.
 *
 * `queue` is the mpmcq structure.
 *
 * Returns mpmcq_entry on success, NULL on cancel;
 */
extern struct mpmcq_entry *mpmcq_pop(struct mpmcq *queue);

/*
 * Cancels any pending pop requests.
 *
 * `queue` is the mpmcq structure.
 */
extern void mpmcq_cancel(struct mpmcq *queue);

#endif
