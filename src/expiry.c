/*
 * =============================================================================
 * expiry.c — Background TTL expiry thread implementation
 * Location: mini-redis/src/expiry.c
 * =============================================================================
 *
 * OS CONCEPTS DEMONSTRATED IN THIS FILE:
 *
 *   Concept 3 — Concurrency Control (pthread + mutex):
 *     This file spawns a BACKGROUND THREAD that runs forever alongside all
 *     client handler threads. It shares the same `db` hash table and MUST
 *     lock db_mutex before touching it — exactly like every client thread does.
 *     This demonstrates that OS-managed threads share memory and require
 *     explicit synchronization to prevent data corruption.
 *
 *   Concept 4 — Data Consistency:
 *     Without db_mutex, the expiry thread could call ht_purge_expired() while
 *     a client thread is in the middle of ht_set(). This is a classic
 *     use-after-free bug: the expiry thread frees an entry while the client
 *     thread still holds a pointer to it. The mutex prevents this entirely —
 *     only one thread can be inside the hash table at any moment.
 *
 * THREAD LIFECYCLE:
 *   1. server.c calls pthread_create(&expiry_tid, NULL, expiry_thread_func, NULL)
 *   2. server.c calls pthread_detach(expiry_tid) — no join needed
 *   3. This thread loops until shutdown_flag is set by SIGINT handler
 *   4. On each iteration: sleep 1s → lock → purge → unlock → repeat
 *   5. When shutdown_flag == 1, the loop exits and the thread terminates
 *      The kernel reclaims it automatically because it was detached.
 * =============================================================================
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>      /* sleep()  */
#include <pthread.h>

#include "expiry.h"
#include "hashtable.h"
#include "globals.h"

/* =============================================================================
 * expiry_thread_func — the background expiry thread's main loop
 *
 * This function IS the thread. pthread_create() calls this as the thread's
 * entry point. It runs concurrently with the main thread and all client
 * handler threads for the entire lifetime of the server.
 *
 * arg: unused — pthread_create requires this signature but we pass NULL.
 * Returns: NULL — required by the pthread_create signature.
 * ============================================================================= */
void *expiry_thread_func(void *arg)
{
    /* Suppress unused parameter warning */
    (void)arg;

    printf("[EXPIRY] Background expiry thread started. "
           "Checking TTLs every 1 second.\n");

    /*
     * Main loop — runs until the server signals shutdown.
     *
     * shutdown_flag is declared volatile sig_atomic_t in server.c (via globals.h).
     * The volatile keyword tells the compiler NOT to cache this in a register —
     * it must read the actual memory address every time. This is essential because
     * the sigint_handler (running in the main thread's signal context) writes to
     * it, and we need to see the update immediately.
     *
     * OS Concept 3: The OS scheduler decides when this thread runs.
     * We call sleep(1) to voluntarily yield CPU, waking once per second.
     */
    while (!shutdown_flag) {

        /*
         * sleep(1) — suspend this thread for 1 second.
         *
         * This is NOT a busy-wait. The OS puts this thread into SLEEPING state,
         * reclaims the CPU for other threads (client handlers), and wakes this
         * thread after 1 second via a timer interrupt.
         *
         * During this 1-second sleep, all client threads run unimpeded —
         * the expiry thread consumes zero CPU while sleeping.
         *
         * Note: sleep() may return early if interrupted by a signal (EINTR).
         * This is fine — we just check shutdown_flag again at the top of the loop.
         */
        sleep(1);

        /*
         * Re-check shutdown_flag after waking — the server may have shut down
         * while we were sleeping. Avoids locking a mutex that may be destroyed.
         */
        if (shutdown_flag)
            break;

        /*
         * ── CRITICAL SECTION ──────────────────────────────────────────────────
         *
         * OS Concept 3: Concurrency Control (mutex lock)
         * OS Concept 4: Data Consistency
         *
         * We MUST hold db_mutex before calling ht_purge_expired().
         *
         * WHY: ht_purge_expired() traverses all buckets and calls free() on
         * expired entries. If a client thread calls ht_get() simultaneously
         * on the same key, it could dereference a pointer that we just freed —
         * this is undefined behaviour (use-after-free), likely a crash or
         * silent data corruption.
         *
         * The mutex guarantees mutual exclusion: while we hold it, no client
         * thread can enter any ht_*() function. All client threads will block
         * on their own pthread_mutex_lock(&db_mutex) calls until we release.
         *
         * This demonstrates the classic producer-consumer synchronization
         * pattern taught in every OS course.
         */
        pthread_mutex_lock(&db_mutex);

        /*
         * ht_purge_expired() is implemented in hashtable.c.
         * It iterates all TABLE_SIZE (256) buckets and for each entry checks:
         *
         *   if (entry->expiry_time != 0 && entry->expiry_time <= time(NULL))
         *       // entry has expired — unlink from chain and free()
         *
         * expiry_time == 0 means "no expiry set" — these entries are never deleted.
         * expiry_time > 0 is a Unix timestamp (seconds since epoch).
         *
         * Example:
         *   Client sends: EXPIRE mykey 30
         *   cmd_expire() calls: ht_set_expiry(db, "mykey", time(NULL) + 30)
         *   30 seconds later, this thread deletes "mykey" automatically.
         */
        ht_purge_expired(db);

        pthread_mutex_unlock(&db_mutex);
        /* ── END CRITICAL SECTION ─────────────────────────────────────────── */
    }

    printf("[EXPIRY] Shutdown flag detected. Expiry thread exiting cleanly.\n");

    /*
     * Return NULL — required by the pthread_create() function signature.
     * Because this thread was pthread_detach()'d in server.c, the kernel
     * automatically frees its stack and thread-local resources on return.
     * We do NOT call pthread_exit() here — return NULL is equivalent and
     * cleaner for functions with a return type.
     */
    return NULL;
}
