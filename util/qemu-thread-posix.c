/*
 * Wrappers around mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Author:
 *  Marcelo Tosatti <mtosatti@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "qemu/osdep.h"
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/futex.h>
#endif
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/notify.h"

static bool name_threads;

void qemu_thread_naming(bool enable)
{
    name_threads = enable;

#ifndef CONFIG_THREAD_SETNAME_BYTHREAD
    /* This is a debugging option, not fatal */
    if (enable) {
        fprintf(stderr, "qemu: thread naming not supported on this host\n");
    }
#endif
}

static void error_exit(int err, const char *msg)
{
    fprintf(stderr, "qemu: %s: %s\n", msg, strerror(err));
    abort();
}

void qemu_mutex_init(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_init(&mutex->lock, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_destroy(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_lock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_lock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

int qemu_mutex_trylock(QemuMutex *mutex)
{
    return pthread_mutex_trylock(&mutex->lock);
}

void qemu_mutex_unlock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_unlock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_rec_mutex_init(QemuRecMutex *mutex)
{
    int err;
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    err = pthread_mutex_init(&mutex->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (err) {
        error_exit(err, __func__);
    }
}

void qemu_cond_init(QemuCond *cond)
{
    int err;

    err = pthread_cond_init(&cond->cond, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_destroy(QemuCond *cond)
{
    int err;

    err = pthread_cond_destroy(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_signal(QemuCond *cond)
{
    int err;

    err = pthread_cond_signal(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    int err;

    err = pthread_cond_broadcast(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
{
    int err;

    err = pthread_cond_wait(&cond->cond, &mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_sem_init(QemuSemaphore *sem, int init)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__)
    rc = pthread_mutex_init(&sem->lock, NULL);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
    rc = pthread_cond_init(&sem->cond, NULL);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
    if (init < 0) {
        error_exit(EINVAL, __func__);
    }
    sem->count = init;
#else
    rc = sem_init(&sem->sem, 0, init);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_sem_destroy(QemuSemaphore *sem)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__)
    rc = pthread_cond_destroy(&sem->cond);
    if (rc < 0) {
        error_exit(rc, __func__);
    }
    rc = pthread_mutex_destroy(&sem->lock);
    if (rc < 0) {
        error_exit(rc, __func__);
    }
#else
    rc = sem_destroy(&sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_sem_post(QemuSemaphore *sem)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__)
    pthread_mutex_lock(&sem->lock);
    if (sem->count == UINT_MAX) {
        rc = EINVAL;
    } else {
        sem->count++;
        rc = pthread_cond_signal(&sem->cond);
    }
    pthread_mutex_unlock(&sem->lock);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
#else
    rc = sem_post(&sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

static void compute_abs_deadline(struct timespec *ts, int ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_nsec = tv.tv_usec * 1000 + (ms % 1000) * 1000000;
    ts->tv_sec = tv.tv_sec + ms / 1000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

int qemu_sem_timedwait(QemuSemaphore *sem, int ms)
{
    int rc;
    struct timespec ts;

#if defined(__APPLE__) || defined(__NetBSD__)
    rc = 0;
    compute_abs_deadline(&ts, ms);
    pthread_mutex_lock(&sem->lock);
    while (sem->count == 0) {
        rc = pthread_cond_timedwait(&sem->cond, &sem->lock, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
        if (rc != 0) {
            error_exit(rc, __func__);
        }
    }
    if (rc != ETIMEDOUT) {
        --sem->count;
    }
    pthread_mutex_unlock(&sem->lock);
    return (rc == ETIMEDOUT ? -1 : 0);
#else
    if (ms <= 0) {
        /* This is cheaper than sem_timedwait.  */
        do {
            rc = sem_trywait(&sem->sem);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1 && errno == EAGAIN) {
            return -1;
        }
    } else {
        compute_abs_deadline(&ts, ms);
        do {
            rc = sem_timedwait(&sem->sem, &ts);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1 && errno == ETIMEDOUT) {
            return -1;
        }
    }
    if (rc < 0) {
        error_exit(errno, __func__);
    }
    return 0;
#endif
}

void qemu_sem_wait(QemuSemaphore *sem)
{
    int rc;

#if defined(__APPLE__) || defined(__NetBSD__)
    pthread_mutex_lock(&sem->lock);
    while (sem->count == 0) {
        rc = pthread_cond_wait(&sem->cond, &sem->lock);
        if (rc != 0) {
            error_exit(rc, __func__);
        }
    }
    --sem->count;
    pthread_mutex_unlock(&sem->lock);
#else
    do {
        rc = sem_wait(&sem->sem);
    } while (rc == -1 && errno == EINTR);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

#ifdef __linux__
#define futex(...)              syscall(__NR_futex, __VA_ARGS__)

static inline void futex_wake(QemuEvent *ev, int n)
{
    futex(ev, FUTEX_WAKE, n, NULL, NULL, 0);
}

static inline void futex_wait(QemuEvent *ev, unsigned val)
{
    while (futex(ev, FUTEX_WAIT, (int) val, NULL, NULL, 0)) {
        switch (errno) {
        case EWOULDBLOCK:
            return;
        case EINTR:
            break; /* get out of switch and retry */
        default:
            abort();
        }
    }
}
#else
static inline void futex_wake(QemuEvent *ev, int n)
{
    pthread_mutex_lock(&ev->lock);
    if (n == 1) {
        pthread_cond_signal(&ev->cond);
    } else {
        pthread_cond_broadcast(&ev->cond);
    }
    pthread_mutex_unlock(&ev->lock);
}

static inline void futex_wait(QemuEvent *ev, unsigned val)
{
    pthread_mutex_lock(&ev->lock);
    if (ev->value == val) {
        pthread_cond_wait(&ev->cond, &ev->lock);
    }
    pthread_mutex_unlock(&ev->lock);
}
#endif

/* Valid transitions:
 * - free->set, when setting the event
 * - busy->set, when setting the event, followed by futex_wake
 * - set->free, when resetting the event
 * - free->busy, when waiting
 *
 * set->busy does not happen (it can be observed from the outside but
 * it really is set->free->busy).
 *
 * busy->free provably cannot happen; to enforce it, the set->free transition
 * is done with an OR, which becomes a no-op if the event has concurrently
 * transitioned to free or busy.
 */

#define EV_SET         0
#define EV_FREE        1
#define EV_BUSY       -1

void qemu_event_init(QemuEvent *ev, bool init)
{
#ifndef __linux__
    pthread_mutex_init(&ev->lock, NULL);
    pthread_cond_init(&ev->cond, NULL);
#endif

    ev->value = (init ? EV_SET : EV_FREE);
}

void qemu_event_destroy(QemuEvent *ev)
{
#ifndef __linux__
    pthread_mutex_destroy(&ev->lock);
    pthread_cond_destroy(&ev->cond);
#endif
}

void qemu_event_set(QemuEvent *ev)
{
    /* qemu_event_set has release semantics, but because it *loads*
     * ev->value we need a full memory barrier here.
     */
    smp_mb();
    if (atomic_read(&ev->value) != EV_SET) {
        if (atomic_xchg(&ev->value, EV_SET) == EV_BUSY) {
            /* There were waiters, wake them up.  */
            futex_wake(ev, INT_MAX);
        }
    }
}

void qemu_event_reset(QemuEvent *ev)
{
    unsigned value;

    value = atomic_read(&ev->value);
    smp_mb_acquire();
    if (value == EV_SET) {
        /*
         * If there was a concurrent reset (or even reset+wait),
         * do nothing.  Otherwise change EV_SET->EV_FREE.
         */
        atomic_or(&ev->value, EV_FREE);
    }
}

void qemu_event_wait(QemuEvent *ev)
{
    unsigned value;

    value = atomic_read(&ev->value);
    smp_mb_acquire();
    if (value != EV_SET) {
        if (value == EV_FREE) {
            /*
             * Leave the event reset and tell qemu_event_set that there
             * are waiters.  No need to retry, because there cannot be
             * a concurrent busy->free transition.  After the CAS, the
             * event will be either set or busy.
             */
            if (atomic_cmpxchg(&ev->value, EV_FREE, EV_BUSY) == EV_SET) {
                return;
            }
        }
        futex_wait(ev, EV_BUSY);
    }
}

static pthread_key_t exit_key;

union NotifierThreadData {
    void *ptr;
    NotifierList list;
};
QEMU_BUILD_BUG_ON(sizeof(union NotifierThreadData) != sizeof(void *));

void qemu_thread_atexit_add(Notifier *notifier)
{
    union NotifierThreadData ntd;
    ntd.ptr = pthread_getspecific(exit_key);
    notifier_list_add(&ntd.list, notifier);
    pthread_setspecific(exit_key, ntd.ptr);
}

void qemu_thread_atexit_remove(Notifier *notifier)
{
    union NotifierThreadData ntd;
    ntd.ptr = pthread_getspecific(exit_key);
    notifier_remove(notifier);
    pthread_setspecific(exit_key, ntd.ptr);
}

static void qemu_thread_atexit_run(void *arg)
{
    union NotifierThreadData ntd = { .ptr = arg };
    notifier_list_notify(&ntd.list, NULL);
}

static void __attribute__((constructor)) qemu_thread_atexit_init(void)
{
    pthread_key_create(&exit_key, qemu_thread_atexit_run);
}


/* Attempt to set the threads name; note that this is for debug, so
 * we're not going to fail if we can't set it.
 */
static void qemu_thread_set_name(QemuThread *thread, const char *name)
{
#ifdef CONFIG_PTHREAD_SETNAME_NP
    pthread_setname_np(thread->thread, name);
#endif
}

void qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void*),
                       void *arg, int mode)
{
    sigset_t set, oldset;
    int err;
    pthread_attr_t attr;

    err = pthread_attr_init(&attr);
    if (err) {
        error_exit(err, __func__);
    }
    if (mode == QEMU_THREAD_DETACHED) {
        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (err) {
            error_exit(err, __func__);
        }
    }

    /* Leave signal handling to the iothread.  */
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &oldset);
    err = pthread_create(&thread->thread, &attr, start_routine, arg);
    if (err)
        error_exit(err, __func__);

    if (name_threads) {
        qemu_thread_set_name(thread, name);
    }

    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    pthread_attr_destroy(&attr);
}

void qemu_thread_get_self(QemuThread *thread)
{
    thread->thread = pthread_self();
}

bool qemu_thread_is_self(QemuThread *thread)
{
   return pthread_equal(pthread_self(), thread->thread);
}

void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}

void *qemu_thread_join(QemuThread *thread)
{
    int err;
    void *ret;

    err = pthread_join(thread->thread, &ret);
    if (err) {
        error_exit(err, __func__);
    }
    return ret;
}

//Avatar-specific
void qemu_avatar_sem_open(QemuAvatarSemaphore *sem, const char *name)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    sem_unlink(name);
    sem_t *rc = sem_open(name, O_CREAT, S_IRUSR | S_IWUSR, 1);

    if(rc == SEM_FAILED) {
        error_exit(errno, __func__);
    }

    sem->sem = rc;
#endif
}

void qemu_avatar_sem_wait(QemuAvatarSemaphore *sem)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    int rc = sem_wait(sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_avatar_sem_post(QemuAvatarSemaphore *sem)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    int rc = sem_post(sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_avatar_mq_open_read(QemuAvatarMessageQueue *mq, const char *name, size_t msg_size)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    mq_unlink(name);

    struct mq_attr attr;
    attr.mq_flags = O_NONBLOCK;
    attr.mq_msgsize = msg_size;
    attr.mq_maxmsg = 10;
    attr.mq_curmsgs = 0;

    mqd_t m = mq_open(name, O_CREAT | O_RDONLY, 0666, &attr);

    if(m == -1)
    {
        error_exit(errno, __func__);
    }

    mq->mq = m;
    mq->valid = true;
#endif
}

void qemu_avatar_mq_open_write(QemuAvatarMessageQueue *mq, const char *name, size_t msg_size)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    mq_unlink(name);

    struct mq_attr attr;
    attr.mq_msgsize = msg_size;
    attr.mq_maxmsg = 10;
    attr.mq_curmsgs = 0;

    mqd_t m = mq_open(name, O_CREAT | O_WRONLY, 0666, &attr);
 
    if(m == -1)
    {
        error_exit(errno, __func__);
    }

    mq->mq = m;
    mq->valid = true;
#endif
}

void qemu_avatar_mq_send(QemuAvatarMessageQueue *mq, void *msg, size_t len)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    int rc = mq_send(mq->mq, msg, len, 0);

    if (rc < 0)
    {
        error_exit(errno, __func__);
    }
#endif
}

int qemu_avatar_mq_receive(QemuAvatarMessageQueue *mq, void *buffer, size_t len)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    int rc = mq_receive(mq->mq, buffer, len, NULL);

    if(errno != EAGAIN)
    {
        return -1;
    }

    if (rc < 0 && errno != EAGAIN)
    {
        error_exit(errno, __func__);
    }

    return rc;
#endif
}

bool qemu_avatar_mq_is_valid(QemuAvatarMessageQueue *mq)
{
    return mq->valid;
}

int qemu_avatar_mq_get_fd(QemuAvatarMessageQueue *mq)
{
    return mq->mq;
}

void qemu_avatar_mq_copy(QemuAvatarMessageQueue *src, QemuAvatarMessageQueue *dst)
{
    dst->mq = src->mq;
    dst->valid = src->valid;
}

void qemu_avatar_mq_close(QemuAvatarMessageQueue *mq)
{
#if defined(__APPLE__) || defined(__NetBSD__)
#else
    if(mq->valid)
    {
        mq_close(mq->mq);
        mq->valid = false;
    }
#endif
}
