/* unit test for the macsync state machine, standalone.
 * suppress the generated protocol header and supply just the enum it wants. */
#define __WINE_WINE_SERVER_PROTOCOL_H
enum inproc_sync_type
{
    INPROC_SYNC_UNKNOWN   = 0,
    INPROC_SYNC_INTERNAL  = 1,
    INPROC_SYNC_EVENT     = 2,
    INPROC_SYNC_MUTEX     = 3,
    INPROC_SYNC_SEMAPHORE = 4,
};

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "macsync.h"

static int failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
                       printf("\n"); failures++; } \
    } while (0)

static void *shared_page(void)
{
    char name[32];
    void *p;
    int fd;

    snprintf( name, sizeof(name), "/mst.%u.%d", (unsigned)getpid(), rand() );
    shm_unlink( name );
    if ((fd = shm_open( name, O_CREAT | O_EXCL | O_RDWR, 0600 )) < 0) { perror("shm_open"); exit(1); }
    shm_unlink( name );
    if (ftruncate( fd, MACSYNC_PAGE_SIZE ) < 0) { perror("ftruncate"); exit(1); }
    p = mmap( NULL, MACSYNC_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    memset( p, 0, MACSYNC_PAGE_SIZE );
    return p;
}

static struct macsync *make( unsigned int type, unsigned int manual, unsigned int count, unsigned int max )
{
    struct macsync *s = shared_page();
    s->magic = MACSYNC_MAGIC;
    s->type = type;
    s->manual = manual;
    s->count = count;
    s->max = max;
    return s;
}

static void test_auto_event(void)
{
    struct macsync *s = make( INPROC_SYNC_EVENT, 0, 0, 0 );
    int ab;

    CHECK( !macsync_try_acquire( s, 1, &ab ), "unsignaled auto event was acquired" );
    CHECK( macsync_set_event( s ) == 0, "prev state should be 0" );
    CHECK( macsync_set_event( s ) == 1, "prev state should be 1" );
    CHECK( macsync_try_acquire( s, 1, &ab ), "signaled auto event not acquired" );
    CHECK( !macsync_try_acquire( s, 1, &ab ), "auto event did not reset on acquire" );

    macsync_set_event( s );
    CHECK( macsync_reset_event( s ) == 1, "reset should report previous 1" );
    CHECK( !macsync_try_acquire( s, 1, &ab ), "reset event was acquired" );
}

static void test_manual_event(void)
{
    struct macsync *s = make( INPROC_SYNC_EVENT, 1, 0, 0 );
    int ab;

    CHECK( !macsync_try_acquire( s, 1, &ab ), "unsignaled manual event was acquired" );
    macsync_set_event( s );
    CHECK( macsync_try_acquire( s, 1, &ab ), "signaled manual event not acquired" );
    CHECK( macsync_try_acquire( s, 1, &ab ), "manual event should stay signaled" );
    CHECK( macsync_try_acquire( s, 2, &ab ), "manual event should satisfy every waiter" );
    macsync_reset_event( s );
    CHECK( !macsync_try_acquire( s, 1, &ab ), "reset manual event was acquired" );
}

static void test_semaphore(void)
{
    struct macsync *s = make( INPROC_SYNC_SEMAPHORE, 0, 2, 3 );
    unsigned int prev;
    int ab;

    CHECK( macsync_try_acquire( s, 1, &ab ), "sem count 2: first acquire" );
    CHECK( macsync_try_acquire( s, 1, &ab ), "sem count 2: second acquire" );
    CHECK( !macsync_try_acquire( s, 1, &ab ), "sem should be exhausted" );

    CHECK( macsync_release_semaphore( s, 3, &prev ) == 0, "release 3 up to max 3" );
    CHECK( prev == 0, "prev count should be 0, got %u", prev );
    CHECK( macsync_release_semaphore( s, 1, &prev ) == -1, "release past max must fail" );
    CHECK( macsync_load( &s->count ) == 3, "failed release must not change count" );

    CHECK( macsync_release_semaphore( s, 0, &prev ) == 0, "release of 0 is legal" );
}

static void test_mutex(void)
{
    struct macsync *s = make( INPROC_SYNC_MUTEX, 0, 0, 0 );
    unsigned int prev;
    int ab;

    CHECK( macsync_try_acquire( s, 10, &ab ), "free mutex not acquired" );
    CHECK( !ab, "fresh mutex should not be abandoned" );
    CHECK( s->owner == 10 && s->count == 1, "owner/count wrong: %u/%u", s->owner, s->count );

    CHECK( macsync_try_acquire( s, 10, &ab ), "recursive acquire failed" );
    CHECK( s->count == 2, "recursion depth should be 2, got %u", s->count );
    CHECK( !macsync_try_acquire( s, 11, &ab ), "held mutex acquired by another thread" );

    CHECK( macsync_release_mutex( s, 11, &prev ) == -1, "non-owner release must fail" );
    CHECK( macsync_release_mutex( s, 10, &prev ) == 0, "owner release failed" );
    CHECK( s->count == 1, "still one level held" );
    CHECK( !macsync_try_acquire( s, 11, &ab ), "still held, should not acquire" );
    CHECK( macsync_release_mutex( s, 10, &prev ) == 0, "final release failed" );
    CHECK( s->owner == 0, "owner should be cleared" );
    CHECK( macsync_release_mutex( s, 10, &prev ) == -1, "release of free mutex must fail" );

    CHECK( macsync_try_acquire( s, 11, &ab ), "released mutex not acquirable" );

    /* the owner dies: server marks it abandoned, next acquirer is told once */
    macsync_store( &s->count, 0 );
    macsync_store( &s->owner, 0 );
    macsync_store( &s->abandoned, 1 );
    CHECK( macsync_try_acquire( s, 12, &ab ), "abandoned mutex not acquirable" );
    CHECK( ab, "abandoned flag not reported" );
    macsync_release_mutex( s, 12, &prev );
    CHECK( macsync_try_acquire( s, 13, &ab ), "acquire after abandon" );
    CHECK( !ab, "abandoned flag must only be reported once" );
}

static void test_undo(void)
{
    unsigned int prev;
    int ab;

    struct macsync *e = make( INPROC_SYNC_EVENT, 0, 1, 0 );
    CHECK( macsync_try_acquire( e, 1, &ab ), "auto event acquire" );
    macsync_undo_acquire( e, 1, ab );
    CHECK( macsync_load( &e->count ) == 1, "auto event not restored by undo" );

    struct macsync *m = make( INPROC_SYNC_EVENT, 1, 1, 0 );
    CHECK( macsync_try_acquire( m, 1, &ab ), "manual event acquire" );
    macsync_undo_acquire( m, 1, ab );
    CHECK( macsync_load( &m->count ) == 1, "manual event must be untouched by undo" );

    struct macsync *s = make( INPROC_SYNC_SEMAPHORE, 0, 2, 5 );
    CHECK( macsync_try_acquire( s, 1, &ab ), "sem acquire" );
    macsync_undo_acquire( s, 1, ab );
    CHECK( macsync_load( &s->count ) == 2, "sem count not restored, got %u", s->count );

    struct macsync *x = make( INPROC_SYNC_MUTEX, 0, 0, 0 );
    CHECK( macsync_try_acquire( x, 7, &ab ), "mutex acquire" );
    macsync_undo_acquire( x, 7, ab );
    CHECK( macsync_load( &x->owner ) == 0, "mutex not released by undo" );
    CHECK( macsync_try_acquire( x, 8, &ab ), "mutex not reusable after undo" );

    macsync_store( &x->count, 0 ); macsync_store( &x->owner, 0 ); macsync_store( &x->abandoned, 1 );
    CHECK( macsync_try_acquire( x, 9, &ab ) && ab, "abandoned acquire" );
    macsync_undo_acquire( x, 9, ab );
    CHECK( macsync_load( &x->abandoned ) == 1, "undo must put the abandoned flag back" );
    (void)prev;
}

/* concurrency: N threads hammer one semaphore, total acquired must equal
 * total released, and the count must never go negative or past max */

struct hammer { struct macsync *sem; int iters; int acquired; };

static void *hammer_thread( void *arg )
{
    struct hammer *h = arg;
    unsigned int tid = (unsigned int)(uintptr_t)pthread_self();
    int ab;

    for (int i = 0; i < h->iters; i++)
    {
        if (macsync_try_acquire( h->sem, tid, &ab ))
        {
            h->acquired++;
            unsigned int prev;
            macsync_release_semaphore( h->sem, 1, &prev );
        }
    }
    return NULL;
}

static void test_semaphore_races(void)
{
    struct macsync *s = make( INPROC_SYNC_SEMAPHORE, 0, 4, 4 );
    struct hammer h[8];
    pthread_t t[8];

    for (int i = 0; i < 8; i++) { h[i].sem = s; h[i].iters = 200000; h[i].acquired = 0; }
    for (int i = 0; i < 8; i++) pthread_create( &t[i], NULL, hammer_thread, &h[i] );
    for (int i = 0; i < 8; i++) pthread_join( t[i], NULL );

    long total = 0;
    for (int i = 0; i < 8; i++) total += h[i].acquired;
    CHECK( macsync_load( &s->count ) == 4, "semaphore leaked: count %u, expected 4", s->count );
    printf( "  semaphore race: %ld acquire/release pairs across 8 threads, final count %u\n",
            total, macsync_load( &s->count ) );
}

/* the mutex must never be held by two threads at once */

struct mtest { struct macsync *m; int iters; volatile int inside; int violations; };

static void *mutex_thread( void *arg )
{
    struct mtest *t = arg;
    unsigned int tid = (unsigned int)(uintptr_t)pthread_self();
    int ab;

    for (int i = 0; i < t->iters; i++)
    {
        if (!macsync_try_acquire( t->m, tid, &ab )) continue;
        if (__atomic_fetch_add( &t->inside, 1, __ATOMIC_SEQ_CST ) != 0) t->violations++;
        __atomic_fetch_sub( &t->inside, 1, __ATOMIC_SEQ_CST );
        unsigned int prev;
        macsync_release_mutex( t->m, tid, &prev );
    }
    return NULL;
}

static void test_mutex_races(void)
{
    struct macsync *m = make( INPROC_SYNC_MUTEX, 0, 0, 0 );
    struct mtest t = { m, 200000, 0, 0 };
    pthread_t th[8];

    for (int i = 0; i < 8; i++) pthread_create( &th[i], NULL, mutex_thread, &t );
    for (int i = 0; i < 8; i++) pthread_join( th[i], NULL );

    CHECK( t.violations == 0, "mutual exclusion violated %d times", t.violations );
    CHECK( macsync_load( &m->owner ) == 0, "mutex left owned by %u", m->owner );
    printf( "  mutex race: 8 threads x 200000, %d exclusion violations\n", t.violations );
}

/* cross-process blocking wait, the thing the whole design rests on */

static void test_cross_process_wait(void)
{
    struct macsync *s = make( INPROC_SYNC_EVENT, 0, 0, 0 );
    pid_t pid;
    int st, ab;

    if (!(pid = fork()))
    {
        unsigned int seq;
        alarm( 5 );
        /* fault the page in before the first shared wait */
        (void)macsync_load( &s->seq );
        for (;;)
        {
            __atomic_fetch_add( &s->waiters, 1, __ATOMIC_SEQ_CST );
            seq = macsync_load( &s->seq );
            if (macsync_try_acquire( s, 99, &ab ))
            {
                __atomic_fetch_sub( &s->waiters, 1, __ATOMIC_SEQ_CST );
                _exit( 0 );
            }
            macsync_wait_addr( &s->seq, seq, MACSYNC_MAX_WAIT_US );
            __atomic_fetch_sub( &s->waiters, 1, __ATOMIC_SEQ_CST );
        }
    }

    usleep( 300000 );
    macsync_set_event( s );
    macsync_changed( s, NULL );

    waitpid( pid, &st, 0 );
    CHECK( WIFEXITED(st) && !WEXITSTATUS(st), "cross-process blocking wait failed (status %d)", st );
    printf( "  cross-process wait: %s\n",
            (WIFEXITED(st) && !WEXITSTATUS(st)) ? "woken correctly" : "FAILED" );
}

int main(void)
{
    setvbuf( stdout, NULL, _IONBF, 0 );

    printf( "macsync state machine\n" );
    test_auto_event();
    test_manual_event();
    test_semaphore();
    test_mutex();
    test_undo();
    printf( "concurrency\n" );
    test_semaphore_races();
    test_mutex_races();
    test_cross_process_wait();

    printf( "\n%s (%d failures)\n", failures ? "FAILED" : "all passed", failures );
    return failures != 0;
}
