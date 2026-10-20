#ifndef LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_KERNEL_H_
#define LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_KERNEL_H_

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <zephyr/sys/util.h>

typedef int64_t k_timeout_t;
typedef uint8_t k_thread_stack_t;
typedef int k_spinlock_key_t;

#define K_FOREVER ((k_timeout_t) - 1)
#define K_NO_WAIT ((k_timeout_t)0)
#define K_MSEC(ms) ((k_timeout_t)(ms))
#define K_PRIO_PREEMPT(priority) (priority)
#define K_THREAD_STACK_DEFINE(name, size) uint8_t name[(size)]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)

struct k_spinlock {
	pthread_mutex_t mutex;
};

struct k_mutex {
	pthread_mutex_t mutex;
};

struct k_event {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	uint32_t events;
};

struct k_sem {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	unsigned int count;
	unsigned int limit;
};

struct k_thread {
	int unused;
};

struct k_timer {
	void (*expiry)(struct k_timer *timer);
	void (*stop)(struct k_timer *timer);
	bool active;
};

typedef struct k_thread *k_tid_t;
typedef void (*k_thread_entry_t)(void *, void *, void *);

extern void (*linkr_debugger_serial_test_sem_take_hook)(void);

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	(void)pthread_mutex_lock(&lock->mutex);
	return 0;
}

static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	(void)key;
	(void)pthread_mutex_unlock(&lock->mutex);
}

static inline void k_mutex_init(struct k_mutex *mutex)
{
	(void)pthread_mutex_init(&mutex->mutex, NULL);
}

static inline int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)timeout;
	return pthread_mutex_lock(&mutex->mutex) == 0 ? 0 : -EIO;
}

static inline int k_mutex_unlock(struct k_mutex *mutex)
{
	return pthread_mutex_unlock(&mutex->mutex) == 0 ? 0 : -EIO;
}

static inline void k_event_init(struct k_event *event)
{
	(void)pthread_mutex_init(&event->mutex, NULL);
	(void)pthread_cond_init(&event->condition, NULL);
	event->events = 0U;
}

static inline uint32_t k_event_post(struct k_event *event, uint32_t events)
{
	uint32_t previous;

	(void)pthread_mutex_lock(&event->mutex);
	previous = event->events;
	event->events |= events;
	(void)pthread_cond_broadcast(&event->condition);
	(void)pthread_mutex_unlock(&event->mutex);
	return previous;
}

static inline uint32_t k_event_wait(struct k_event *event, uint32_t events,
				    bool reset, k_timeout_t timeout)
{
	uint32_t result;

	(void)timeout;
	(void)pthread_mutex_lock(&event->mutex);
	while ((event->events & events) == 0U) {
		(void)pthread_cond_wait(&event->condition, &event->mutex);
	}
	result = event->events & events;
	if (reset) {
		event->events &= ~result;
	}
	(void)pthread_mutex_unlock(&event->mutex);
	return result;
}

static inline uint32_t k_event_wait_safe(struct k_event *event, uint32_t events,
					 bool reset, k_timeout_t timeout)
{
	(void)reset;
	return k_event_wait(event, events, true, timeout);
}

static inline void k_sem_init(struct k_sem *sem, unsigned int initial_count,
			      unsigned int limit)
{
	(void)pthread_mutex_init(&sem->mutex, NULL);
	(void)pthread_cond_init(&sem->condition, NULL);
	sem->count = initial_count;
	sem->limit = limit;
}

static inline void k_sem_reset(struct k_sem *sem)
{
	(void)pthread_mutex_lock(&sem->mutex);
	sem->count = 0U;
	(void)pthread_mutex_unlock(&sem->mutex);
}

static inline void k_sem_give(struct k_sem *sem)
{
	(void)pthread_mutex_lock(&sem->mutex);
	if (sem->count < sem->limit) {
		sem->count++;
	}
	(void)pthread_cond_signal(&sem->condition);
	(void)pthread_mutex_unlock(&sem->mutex);
}

static inline int k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	int ret = 0;

	if (linkr_debugger_serial_test_sem_take_hook != NULL) {
		void (*hook)(void) = linkr_debugger_serial_test_sem_take_hook;

		linkr_debugger_serial_test_sem_take_hook = NULL;
		hook();
	}
	(void)pthread_mutex_lock(&sem->mutex);
	if (timeout == K_NO_WAIT && sem->count == 0U) {
		ret = -EAGAIN;
	} else if (timeout == K_FOREVER) {
		while (sem->count == 0U) {
			(void)pthread_cond_wait(&sem->condition, &sem->mutex);
		}
	} else if (sem->count == 0U) {
		struct timespec deadline;

		(void)clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += timeout / 1000;
		deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		while (sem->count == 0U) {
			int wait_ret = pthread_cond_timedwait(
			    &sem->condition, &sem->mutex, &deadline);

			if (wait_ret == ETIMEDOUT) {
				ret = -EAGAIN;
				break;
			}
		}
	}
	if (ret == 0 && sem->count != 0U) {
		sem->count--;
	}
	(void)pthread_mutex_unlock(&sem->mutex);
	return ret;
}

static inline void k_timer_init(struct k_timer *timer,
				void (*expiry)(struct k_timer *timer),
				void (*stop)(struct k_timer *timer))
{
	timer->expiry = expiry;
	timer->stop = stop;
	timer->active = false;
}

static inline void k_timer_start(struct k_timer *timer, k_timeout_t duration,
				 k_timeout_t period)
{
	(void)duration;
	(void)period;
	timer->active = true;
}

static inline void k_timer_stop(struct k_timer *timer)
{
	timer->active = false;
}

static inline k_tid_t
k_thread_create(struct k_thread *thread, k_thread_stack_t *stack,
		size_t stack_size, k_thread_entry_t entry, void *p1, void *p2,
		void *p3, int priority, uint32_t options, k_timeout_t delay)
{
	(void)stack;
	(void)stack_size;
	(void)entry;
	(void)p1;
	(void)p2;
	(void)p3;
	(void)priority;
	(void)options;
	(void)delay;
	return thread;
}

static inline void k_sleep(k_timeout_t timeout) { (void)timeout; }
static inline int64_t k_uptime_get(void) { return 0; }
static inline int k_thread_name_set(struct k_thread *thread, const char *name)
{
	(void)thread;
	(void)name;
	return 0;
}

#endif
