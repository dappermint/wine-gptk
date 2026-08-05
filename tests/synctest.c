/* correctness and throughput check for wine's sync primitives.
 *
 * written to test the msync/esync backends, which replace the wineserver round
 * trip for events, semaphores and mutexes.  a backend that is merely broken is
 * easy to spot; one that is subtly wrong shows up as a lost wakeup or a count
 * that does not add up, which is why every phase here asserts an invariant
 * rather than just timing a loop.
 *
 * run the same binary with and without WINEMSYNC=1 and compare both the
 * PASS/FAIL lines and the timings.
 *
 * usage: synctest.exe [iterations]   (default 20000)
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 8

static LONG  counter;
static DWORD iterations = 20000;
static int   failures;

static HANDLE sem, mutex, ev_auto, ev_manual, done_ev;

static void check(const char *what, int ok)
{
    printf("%-34s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static ULONGLONG now_ms(void)
{
    return GetTickCount64();
}

/* semaphore + mutex: every thread takes a slot, bumps a shared counter under
 * the mutex and releases.  the final count is the invariant: a lost release or
 * a double wake shows up immediately. */
/* report the first bad return rather than just bailing: a backend that fails
 * fast is indistinguishable from one that never ran unless the code is shown */
static LONG  first_err_reported;
static DWORD first_err_ret, first_err_gle;
static const char *first_err_where = "";

static void note_bad_wait(const char *where, DWORD ret)
{
    if (InterlockedCompareExchange(&first_err_reported, 1, 0) != 0) return;
    first_err_where = where;
    first_err_ret = ret;
    first_err_gle = GetLastError();
}

static DWORD WINAPI sem_worker(void *arg)
{
    DWORD i, r;

    (void)arg;
    for (i = 0; i < iterations; i++)
    {
        if ((r = WaitForSingleObject(sem, 10000)) != WAIT_OBJECT_0)
        {
            note_bad_wait("WaitForSingleObject(semaphore)", r);
            return 1;
        }
        if ((r = WaitForSingleObject(mutex, 10000)) != WAIT_OBJECT_0)
        {
            note_bad_wait("WaitForSingleObject(mutex)", r);
            return 1;
        }
        counter++;
        if (!ReleaseMutex(mutex)) note_bad_wait("ReleaseMutex", GetLastError());
        if (!ReleaseSemaphore(sem, 1, NULL)) note_bad_wait("ReleaseSemaphore", GetLastError());
    }
    return 0;
}

/* auto-reset event ping-pong: exactly one waiter may wake per set.  if the
 * backend wakes two, the count overshoots. */
static DWORD WINAPI event_worker(void *arg)
{
    DWORD r;

    (void)arg;
    for (;;)
    {
        if ((r = WaitForSingleObject(ev_auto, 5000)) != WAIT_OBJECT_0)
        {
            note_bad_wait("WaitForSingleObject(auto event)", r);
            return 1;
        }
        if (InterlockedIncrement(&counter) >= (LONG)iterations)
        {
            SetEvent(done_ev);
            return 0;
        }
    }
}

/* WaitForMultipleObjects with bWaitAll, plus a timeout path.  the timeout is
 * the case an eventfd/mach based backend most often gets wrong. */
static void test_multi(void)
{
    HANDLE objs[2] = { ev_manual, done_ev };
    DWORD r;

    ResetEvent(ev_manual);
    ResetEvent(done_ev);

    r = WaitForMultipleObjects(2, objs, TRUE, 200);
    check("WaitAll times out when unsignalled", r == WAIT_TIMEOUT);

    SetEvent(ev_manual);
    r = WaitForMultipleObjects(2, objs, FALSE, 200);
    check("WaitAny returns the signalled one", r == WAIT_OBJECT_0);

    SetEvent(done_ev);
    r = WaitForMultipleObjects(2, objs, TRUE, 2000);
    check("WaitAll returns when both signalled", r == WAIT_OBJECT_0);

    r = WaitForSingleObject(ev_manual, 0);
    check("manual reset event stays signalled", r == WAIT_OBJECT_0);

    ResetEvent(ev_manual);
    r = WaitForSingleObject(ev_manual, 0);
    check("manual reset event resets", r == WAIT_TIMEOUT);
}

/* an abandoned mutex has to be reported, or a game that crashes while holding
 * one deadlocks every other process in the prefix */
static DWORD WINAPI abandon_worker(void *arg)
{
    WaitForSingleObject((HANDLE)arg, 5000);
    return 0;  /* deliberately exits without releasing */
}

static void test_abandoned(void)
{
    HANDLE m = CreateMutexA(NULL, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, abandon_worker, m, 0, NULL);
    DWORD r;

    WaitForSingleObject(t, 5000);
    CloseHandle(t);

    r = WaitForSingleObject(m, 1000);
    check("abandoned mutex reported", r == WAIT_ABANDONED);
    if (r == WAIT_ABANDONED || r == WAIT_OBJECT_0) ReleaseMutex(m);
    CloseHandle(m);
}

int main(int argc, char **argv)
{
    HANDLE threads[THREADS];
    ULONGLONG t0;
    DWORD i;

    if (argc > 1) iterations = strtoul(argv[1], NULL, 10);
    printf("synctest: %u iterations x %d threads\n", (unsigned)iterations, THREADS);
    printf("WINEMSYNC=%s WINEESYNC=%s\n",
           getenv("WINEMSYNC") ? getenv("WINEMSYNC") : "unset",
           getenv("WINEESYNC") ? getenv("WINEESYNC") : "unset");

    sem       = CreateSemaphoreA(NULL, 4, 4, NULL);
    mutex     = CreateMutexA(NULL, FALSE, NULL);
    ev_auto   = CreateEventA(NULL, FALSE, FALSE, NULL);
    ev_manual = CreateEventA(NULL, TRUE, FALSE, NULL);
    done_ev   = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!sem || !mutex || !ev_auto || !ev_manual || !done_ev)
    {
        fprintf(stderr, "object creation failed: %lu\n", GetLastError());
        return 2;
    }

    test_multi();
    test_abandoned();

    counter = 0;
    t0 = now_ms();
    for (i = 0; i < THREADS; i++) threads[i] = CreateThread(NULL, 0, sem_worker, NULL, 0, NULL);
    for (i = 0; i < THREADS; i++) WaitForSingleObject(threads[i], 120000);
    for (i = 0; i < THREADS; i++) CloseHandle(threads[i]);
    check("semaphore+mutex count exact", counter == (LONG)(iterations * THREADS));
    printf("  semaphore+mutex: %llu ms for %u ops\n",
           (unsigned long long)(now_ms() - t0), (unsigned)(iterations * THREADS));

    counter = 0;
    ResetEvent(done_ev);
    t0 = now_ms();
    for (i = 0; i < THREADS; i++) threads[i] = CreateThread(NULL, 0, event_worker, NULL, 0, NULL);
    for (i = 0; i < iterations + THREADS; i++)
    {
        SetEvent(ev_auto);
        if (WaitForSingleObject(done_ev, 0) == WAIT_OBJECT_0) break;
    }
    WaitForSingleObject(done_ev, 30000);
    for (i = 0; i < THREADS; i++) SetEvent(ev_auto);
    for (i = 0; i < THREADS; i++) WaitForSingleObject(threads[i], 10000);
    for (i = 0; i < THREADS; i++) CloseHandle(threads[i]);
    check("auto event woke no more than set", counter <= (LONG)(iterations + THREADS));
    printf("  auto event: %llu ms, counter %ld\n",
           (unsigned long long)(now_ms() - t0), (long)counter);

    if (first_err_reported)
        printf("first bad wait: %s returned 0x%08lx, GetLastError=%lu\n",
               first_err_where, (unsigned long)first_err_ret, (unsigned long)first_err_gle);

    printf("RESULT: %s (%d failures)\n", failures ? "BROKEN" : "ok", failures);
    return failures ? 1 : 0;
}
