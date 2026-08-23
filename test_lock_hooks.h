#include <pthread.h>

#define PA_LOCK_TYPE pthread_mutex_t
#define PA_LOCK_INIT(lock) ((void)pthread_mutex_init((lock), NULL))
#define PA_LOCK_ACQUIRE(lock) ((void)pthread_mutex_lock(lock))
#define PA_LOCK_RELEASE(lock) ((void)pthread_mutex_unlock(lock))
