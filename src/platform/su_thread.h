/* Minimal threads for the job pool and tests: mutex, condition variable, create and join.
 * POSIX maps onto pthreads; Windows onto SRWLOCK, CONDITION_VARIABLE and _beginthreadex.
 * The Windows types are opaque pointer-sized words, so this header pulls in no windows.h. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_PLATFORM_SU_THREAD_H
#define SUTEKH_PLATFORM_SU_THREAD_H

#if defined(_WIN32)
typedef struct SuMutex {
    void *opaque; /* SRWLOCK */
} SuMutex;
typedef struct SuCond {
    void *opaque; /* CONDITION_VARIABLE */
} SuCond;
typedef struct SuThread {
    void *handle; /* HANDLE from _beginthreadex */
} SuThread;
#else
#include <pthread.h>
typedef struct SuMutex {
    pthread_mutex_t m;
} SuMutex;
typedef struct SuCond {
    pthread_cond_t c;
} SuCond;
typedef struct SuThread {
    pthread_t t;
} SuThread;
#endif

typedef void *(*SuThreadFn)(void *arg);

/* Each returns 0 on success and nonzero on failure (as the pthread functions do). */
int su_mutex_init(SuMutex *m);
void su_mutex_destroy(SuMutex *m);
void su_mutex_lock(SuMutex *m);
void su_mutex_unlock(SuMutex *m);
int su_cond_init(SuCond *c);
void su_cond_destroy(SuCond *c);
/* Releases m, waits, reacquires m. Spurious wakeups are possible; callers loop on a predicate. */
void su_cond_wait(SuCond *c, SuMutex *m);
void su_cond_signal(SuCond *c);
void su_cond_broadcast(SuCond *c);
/* Starts fn(arg) on a new thread. The return value of fn is discarded by su_thread_join. */
int su_thread_create(SuThread *t, SuThreadFn fn, void *arg);
int su_thread_join(SuThread t);

#endif
