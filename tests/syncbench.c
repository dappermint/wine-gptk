/* measure the thing msync exists to speed up: how long a synchronisation
 * round trip takes. every one of these is a wineserver RPC without it. */
#include <windows.h>
#include <stdio.h>

static HANDLE ping, pong;
static volatile LONG go;

#define ITERS 20000

static DWORD WINAPI responder( void *arg )
{
    for (int i = 0; i < ITERS; i++)
    {
        WaitForSingleObject( ping, INFINITE );
        SetEvent( pong );
    }
    return 0;
}

static double elapsed_ms( LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f )
{
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

int main( void )
{
    LARGE_INTEGER freq, t0, t1;
    HANDLE thread, sem, mutex;
    double ms;

    QueryPerformanceFrequency( &freq );

    /* 1. uncontended set/reset on one event */
    ping = CreateEventW( NULL, FALSE, FALSE, NULL );
    QueryPerformanceCounter( &t0 );
    for (int i = 0; i < ITERS; i++)
    {
        SetEvent( ping );
        WaitForSingleObject( ping, 0 );
    }
    QueryPerformanceCounter( &t1 );
    ms = elapsed_ms( t0, t1, freq );
    printf( "event set+wait, uncontended : %8.2f ms  %7.0f ns/op\n", ms, ms * 1e6 / ITERS );

    /* 2. two threads ping-ponging: a real blocking round trip each way */
    pong = CreateEventW( NULL, FALSE, FALSE, NULL );
    thread = CreateThread( NULL, 0, responder, NULL, 0, NULL );
    QueryPerformanceCounter( &t0 );
    for (int i = 0; i < ITERS; i++)
    {
        SetEvent( ping );
        WaitForSingleObject( pong, INFINITE );
    }
    QueryPerformanceCounter( &t1 );
    ms = elapsed_ms( t0, t1, freq );
    printf( "event ping-pong, 2 threads  : %8.2f ms  %7.0f ns/round trip\n", ms, ms * 1e6 / ITERS );
    WaitForSingleObject( thread, INFINITE );

    /* 3. semaphore release/acquire */
    sem = CreateSemaphoreW( NULL, 0, ITERS + 1, NULL );
    QueryPerformanceCounter( &t0 );
    for (int i = 0; i < ITERS; i++)
    {
        ReleaseSemaphore( sem, 1, NULL );
        WaitForSingleObject( sem, INFINITE );
    }
    QueryPerformanceCounter( &t1 );
    ms = elapsed_ms( t0, t1, freq );
    printf( "semaphore release+acquire   : %8.2f ms  %7.0f ns/op\n", ms, ms * 1e6 / ITERS );

    /* 4. mutex acquire/release, the lock a game engine leans on */
    mutex = CreateMutexW( NULL, FALSE, NULL );
    QueryPerformanceCounter( &t0 );
    for (int i = 0; i < ITERS; i++)
    {
        WaitForSingleObject( mutex, INFINITE );
        ReleaseMutex( mutex );
    }
    QueryPerformanceCounter( &t1 );
    ms = elapsed_ms( t0, t1, freq );
    printf( "mutex acquire+release       : %8.2f ms  %7.0f ns/op\n", ms, ms * 1e6 / ITERS );

    /* 5. wait on several objects at once, the path that cannot use a
     *    per-object futex and falls back to the shared counter */
    {
        HANDLE objs[4] = { ping, pong, sem, mutex };
        QueryPerformanceCounter( &t0 );
        for (int i = 0; i < ITERS; i++)
        {
            SetEvent( ping );
            WaitForMultipleObjects( 4, objs, FALSE, INFINITE );
        }
        QueryPerformanceCounter( &t1 );
        ms = elapsed_ms( t0, t1, freq );
        printf( "WaitForMultipleObjects(4)   : %8.2f ms  %7.0f ns/op\n", ms, ms * 1e6 / ITERS );
    }

    printf( "\n%d iterations each\n", ITERS );
    return 0;
}
