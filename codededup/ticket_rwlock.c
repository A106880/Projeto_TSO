#include "ticket_rwlock.h"

void ticket_rwlock_init(ticket_rwlock_t *lock) {
    lock->next_ticket = 0;
    lock->now_serving = 0;
    lock->readers_active = 0;
    pthread_mutex_init(&lock->mutex, NULL);
    pthread_cond_init(&lock->cond_readers, NULL);
    pthread_cond_init(&lock->cond_writers, NULL);
}

void ticket_rwlock_destroy(ticket_rwlock_t *lock) {
    pthread_mutex_destroy(&lock->mutex);
    pthread_cond_destroy(&lock->cond_readers);
    pthread_cond_destroy(&lock->cond_writers);
}

void ticket_rwlock_read_lock(ticket_rwlock_t *lock) {
    pthread_mutex_lock(&lock->mutex);
    unsigned long long my_ticket = lock->next_ticket++;
    
    while (my_ticket != lock->now_serving) {
        pthread_cond_wait(&lock->cond_readers, &lock->mutex);
    }
    
    lock->readers_active++;
    lock->now_serving++;
    
    pthread_cond_broadcast(&lock->cond_readers);
    
    pthread_mutex_unlock(&lock->mutex);
}

void ticket_rwlock_read_unlock(ticket_rwlock_t *lock) {
    pthread_mutex_lock(&lock->mutex);
    lock->readers_active--;
    
    if (lock->readers_active == 0) {
        pthread_cond_broadcast(&lock->cond_writers);
    }
    
    pthread_mutex_unlock(&lock->mutex);
}

void ticket_rwlock_write_lock(ticket_rwlock_t *lock) {
    pthread_mutex_lock(&lock->mutex);
    unsigned long long my_ticket = lock->next_ticket++;
    
    while (my_ticket != lock->now_serving || lock->readers_active > 0) {
        pthread_cond_wait(&lock->cond_writers, &lock->mutex);
    }
    
    pthread_mutex_unlock(&lock->mutex);
}

void ticket_rwlock_write_unlock(ticket_rwlock_t *lock) {
    pthread_mutex_lock(&lock->mutex);
    
    lock->now_serving++;
    
    pthread_cond_broadcast(&lock->cond_readers);
    pthread_cond_broadcast(&lock->cond_writers);
    
    pthread_mutex_unlock(&lock->mutex);
}
