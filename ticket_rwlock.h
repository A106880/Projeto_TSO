#ifndef TICKET_RWLOCK_H
#define TICKET_RWLOCK_H

#include <pthread.h>

typedef struct {
    unsigned long long next_ticket;
    unsigned long long now_serving;
    unsigned int readers_active;
    pthread_mutex_t mutex;
    pthread_cond_t cond_readers;
    pthread_cond_t cond_writers;
} ticket_rwlock_t;

void ticket_rwlock_init(ticket_rwlock_t *lock);
void ticket_rwlock_destroy(ticket_rwlock_t *lock);
void ticket_rwlock_read_lock(ticket_rwlock_t *lock);
void ticket_rwlock_read_unlock(ticket_rwlock_t *lock);
void ticket_rwlock_write_lock(ticket_rwlock_t *lock);
void ticket_rwlock_write_unlock(ticket_rwlock_t *lock);

#endif
