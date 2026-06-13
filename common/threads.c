/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "threads.h"
#include "cmdlib.h"
#include "connect.h"

#define MAX_THREADS 64

int workcount;
int dispatch;
int completed;
long long total_work;
long long completed_work;
int oldf;
qboolean pacifier;

qboolean threaded;

int GetThreadWork(void)
{
    int r;

    ThreadLock();

    if (dispatch == workcount)
    {
        ThreadUnlock();
        return -1;
    }

    r = dispatch;
    dispatch++;
    ThreadUnlock();

    return r;
}

void ThreadCompletedWeighted(int weight)
{
    int f;

    ThreadLock();
    completed++;
    completed_work += weight;

    if (total_work > 0)
    {
        f = (int)(10 * completed_work / total_work);
    }
    else
    {
        f = 10 * completed / workcount;
    }

    if (f != oldf)
    {
        oldf = f;
        if (pacifier)
        {
            _printf("%i...", f);
        }
    }
    else
    {
        Broadcast_KeepAlive();
    }
    ThreadUnlock();
}

void ThreadCompleted(void)
{
    ThreadCompletedWeighted(1);
}

void (*workfunction)(int);

void ThreadWorkerFunction(int threadnum)
{
    int work;

    while (1)
    {
        work = GetThreadWork();
        if (work == -1)
            break;
        //_printf ("thread %i, work %i\n", threadnum, work);
        workfunction(work);
        ThreadCompleted();
    }
}

void RunThreadsOnIndividual(int workcnt, qboolean showpacifier,
                            void (*func)(int))
{
    if (numthreads == -1)
        ThreadSetDefault();
    workfunction = func;
    total_work = 0; // standard progress
    RunThreadsOn(workcnt, showpacifier, ThreadWorkerFunction);
}

void RunThreadsOnWeighted(int workcnt, long long total_w, qboolean showpacifier,
                          void (*func)(int))
{
    if (numthreads == -1)
        ThreadSetDefault();
    workfunction = func;
    total_work = total_w;
    RunThreadsOn(workcnt, showpacifier, ThreadWorkerFunction);
}

/*
===================================================================

WIN32

===================================================================
*/
#ifdef _WIN32

#define USED

#include <windows.h>

int numthreads = -1;
CRITICAL_SECTION crit;
static int enter;

void ThreadSetDefault(void)
{
    SYSTEM_INFO info;

    if (numthreads == -1) // not set manually
    {
        GetSystemInfo(&info);
        numthreads = info.dwNumberOfProcessors;
        if (numthreads < 1 || numthreads > 32)
            numthreads = 1;
    }

    _printf("%i threads\n", numthreads);
}

void ThreadLock(void)
{
    if (!threaded)
        return;
    EnterCriticalSection(&crit);
    if (enter)
        Error("Recursive ThreadLock\n");
    enter = 1;
}

void ThreadUnlock(void)
{
    if (!threaded)
        return;
    if (!enter)
        Error("ThreadUnlock without lock\n");
    enter = 0;
    LeaveCriticalSection(&crit);
}

/*
=============
RunThreadsOn
=============
*/
void RunThreadsOn(int workcnt, qboolean showpacifier, void (*func)(int))
{
    DWORD threadid[MAX_THREADS];
    HANDLE threadhandle[MAX_THREADS];
    int i;
    int start, end;

    start = I_FloatTime();
    dispatch = 0;
    completed = 0;
    completed_work = 0;
    workcount = workcnt;
    oldf = -1;
    pacifier = showpacifier;
    threaded = qtrue;

    //
    // run threads in parallel
    //
    InitializeCriticalSection(&crit);

    if (numthreads == 1)
    { // use same thread
        func(0);
    }
    else
    {
        for (i = 0; i < numthreads; i++)
        {
            threadhandle[i] = CreateThread(
                NULL,                         // LPSECURITY_ATTRIBUTES lpsa,
                0,                            // DWORD cbStack,
                (LPTHREAD_START_ROUTINE)func, // LPTHREAD_START_ROUTINE lpStartAddr,
                (LPVOID)(intptr_t)i,          // LPVOID lpvThreadParm,
                0,                            //   DWORD fdwCreate,
                &threadid[i]);
        }

        for (i = 0; i < numthreads; i++)
            WaitForSingleObject(threadhandle[i], INFINITE);
    }
    DeleteCriticalSection(&crit);

    threaded = qfalse;
    end = I_FloatTime();
    if (pacifier)
        _printf(" (%i) ", end - start);
}

#endif

/*
===================================================================

LINUX / UNIX / MAC

===================================================================
*/

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#define USED

#include <pthread.h>
#include <unistd.h>

int numthreads = -1;

void ThreadSetDefault(void)
{
    if (numthreads == -1) // not set manually
    {
#ifdef _SC_NPROCESSORS_ONLN
        numthreads = sysconf(_SC_NPROCESSORS_ONLN);
#else
        numthreads = 4;
#endif
        if (numthreads < 1 || numthreads > MAX_THREADS)
            numthreads = 1;
    }
    _printf("%i threads\n", numthreads);
}

pthread_mutex_t *my_mutex;

void ThreadLock(void)
{
    if (my_mutex)
        pthread_mutex_lock(my_mutex);
}

void ThreadUnlock(void)
{
    if (my_mutex)
        pthread_mutex_unlock(my_mutex);
}

/*
=============
RunThreadsOn
=============
*/
void RunThreadsOn(int workcnt, qboolean showpacifier, void (*func)(int))
{
    int i;
    pthread_t work_threads[MAX_THREADS];
    pthread_attr_t attrib;
    pthread_mutexattr_t mattrib;
    int start, end;

    start = I_FloatTime();
    dispatch = 0;
    completed = 0;
    completed_work = 0;
    workcount = workcnt;
    oldf = -1;
    pacifier = showpacifier;
    threaded = qtrue;

    if (pacifier)
        setbuf(stdout, NULL);

    if (!my_mutex)
    {
        my_mutex = malloc(sizeof(*my_mutex));
        if (pthread_mutexattr_init(&mattrib) != 0)
            Error("pthread_mutexattr_init failed");
#ifdef __linux__
        pthread_mutexattr_settype(&mattrib, PTHREAD_MUTEX_ADAPTIVE_NP);
#endif
        if (pthread_mutex_init(my_mutex, &mattrib) != 0)
            Error("pthread_mutex_init failed");
    }

    if (pthread_attr_init(&attrib) != 0)
        Error("pthread_attr_init failed");
    if (pthread_attr_setstacksize(&attrib, 0x100000) != 0)
        Error("pthread_attr_setstacksize failed");

    if (numthreads == 1)
    {
        func(0);
    }
    else
    {
        for (i = 0; i < numthreads; i++)
        {
            if (pthread_create(&work_threads[i], &attrib, (void *(*)(void *))func,
                               (void *)(intptr_t)i) != 0)
                Error("pthread_create failed");
        }

        for (i = 0; i < numthreads; i++)
        {
            if (pthread_join(work_threads[i], NULL) != 0)
                Error("pthread_join failed");
        }
    }

    threaded = qfalse;

    end = I_FloatTime();
    if (pacifier)
        _printf(" (%i)\n", end - start);
}

#endif

/*
===================================================================

IRIX

===================================================================
*/

#ifdef _MIPS_ISA
#define USED

#include <abi_mutex.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <task.h>

int numthreads = -1;
abilock_t lck;

void ThreadSetDefault(void)
{
    if (numthreads == -1)
        numthreads = prctl(PR_MAXPPROCS);
    _printf("%i threads\n", numthreads);
    usconfig(CONF_INITUSERS, numthreads);
}

void ThreadLock(void) { spin_lock(&lck); }

void ThreadUnlock(void) { release_lock(&lck); }

/*
=============
RunThreadsOn
=============
*/
void RunThreadsOn(int workcnt, qboolean showpacifier, void (*func)(int))
{
    int i;
    int pid[MAX_THREADS];
    int start, end;

    start = I_FloatTime();
    dispatch = 0;
    workcount = workcnt;
    oldf = -1;
    pacifier = showpacifier;
    threaded = qtrue;

    if (pacifier)
        setbuf(stdout, NULL);

    init_lock(&lck);

    for (i = 0; i < numthreads - 1; i++)
    {
        pid[i] = sprocsp((void (*)(void *, size_t))func, PR_SALL, (void *)i, NULL,
                         0x200000); // 2 meg stacks
        if (pid[i] == -1)
        {
            perror("sproc");
            Error("sproc failed");
        }
    }

    func(i);

    for (i = 0; i < numthreads - 1; i++)
        wait(NULL);

    threaded = qfalse;

    end = I_FloatTime();
    if (pacifier)
        _printf(" (%i)\n", end - start);
}

#endif

/*
=======================================================================

  SINGLE THREAD

=======================================================================
*/

#ifndef USED

int numthreads = 1;

void ThreadSetDefault(void) { numthreads = 1; }

void ThreadLock(void) {}

void ThreadUnlock(void) {}

/*
=============
RunThreadsOn
=============
*/
void RunThreadsOn(int workcnt, qboolean showpacifier, void (*func)(int))
{
    int i;
    int start, end;

    dispatch = 0;
    workcount = workcnt;
    oldf = -1;
    pacifier = showpacifier;
    start = I_FloatTime();
    func(0);

    end = I_FloatTime();
    if (pacifier)
        _printf(" (%i)\n", end - start);
}

#endif
