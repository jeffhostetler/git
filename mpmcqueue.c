#include "mpmcqueue.h"

void mpmcq_init(struct mpmcq *queue)
{
	queue->head = NULL;
	queue->cancel = 0;
	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->condition, NULL);
}

void mpmcq_destroy(struct mpmcq *queue)
{
	pthread_mutex_destroy(&queue->mutex);
	pthread_cond_destroy(&queue->condition);
}

void mpmcq_push(struct mpmcq *queue, struct mpmcq_entry *entry)
{
	pthread_mutex_lock(&queue->mutex);
	entry->next = queue->head;
	queue->head = entry;
	pthread_cond_signal(&queue->condition);
	pthread_mutex_unlock(&queue->mutex);
}

struct mpmcq_entry *mpmcq_pop(struct mpmcq *queue)
{
	struct mpmcq_entry *entry = NULL;

	pthread_mutex_lock(&queue->mutex);
	while (!queue->head && !queue->cancel)
		pthread_cond_wait(&queue->condition, &queue->mutex);
	if (!queue->cancel) {
		entry = queue->head;
		queue->head = entry->next;
	}
	pthread_mutex_unlock(&queue->mutex);
	return entry;
}

void mpmcq_cancel(struct mpmcq *queue)
{
	struct mpmcq_entry *entry;

	pthread_mutex_lock(&queue->mutex);
	queue->cancel = 1;
	pthread_cond_broadcast(&queue->condition);
	pthread_mutex_unlock(&queue->mutex);
}
