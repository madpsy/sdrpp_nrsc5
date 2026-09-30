// The part of POSIX threads nrsc5 uses, on Windows primitives.
//
// Mutexes are SRW locks, whose static initializer is all zeros, so
// PTHREAD_MUTEX_INITIALIZER works for nrsc5's global FFTW mutex. Threads are
// only started for nrsc5's RTL-SDR worker, which a pipe never uses; they are
// still implemented, so nothing links to a stub that would misbehave.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static __inline int pthread_mutex_init(pthread_mutex_t* m, const void* attr) { (void)attr; InitializeSRWLock(m); return 0; }
static __inline int pthread_mutex_destroy(pthread_mutex_t* m) { (void)m; return 0; }
static __inline int pthread_mutex_lock(pthread_mutex_t* m) { AcquireSRWLockExclusive(m); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t* m) { ReleaseSRWLockExclusive(m); return 0; }

static __inline int pthread_cond_init(pthread_cond_t* c, const void* attr) { (void)attr; InitializeConditionVariable(c); return 0; }
static __inline int pthread_cond_destroy(pthread_cond_t* c) { (void)c; return 0; }
static __inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1; }
static __inline int pthread_cond_signal(pthread_cond_t* c) { WakeConditionVariable(c); return 0; }
static __inline int pthread_cond_broadcast(pthread_cond_t* c) { WakeAllConditionVariable(c); return 0; }

typedef struct {
    void* (*fn)(void*);
    void* arg;
} pthread_start_t_;

static unsigned __stdcall pthread_trampoline_(void* p) {
    pthread_start_t_ s = *(pthread_start_t_*)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

static __inline int pthread_create(pthread_t* t, const void* attr, void* (*fn)(void*), void* arg) {
    (void)attr;
    pthread_start_t_* s = (pthread_start_t_*)malloc(sizeof(*s));
    if (!s) { return -1; }
    s->fn = fn;
    s->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, pthread_trampoline_, s, 0, NULL);
    if (!*t) {
        free(s);
        return -1;
    }
    return 0;
}

static __inline int pthread_join(pthread_t t, void** ret) {
    if (ret) { *ret = NULL; }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}
