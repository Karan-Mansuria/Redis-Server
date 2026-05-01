/*
 * THREAD LIFECYCLE:
 *   1. server.c calls pthread_create(&expiry_tid, NULL, expiry_thread_func, NULL)
 *   2. server.c calls pthread_detach(expiry_tid) — no join needed
 *   3. This thread loops until shutdown_flag is set by SIGINT handler
 *   4. On each iteration: sleep 1s → lock → purge → unlock → repeat
 *   5. When shutdown_flag == 1, the loop exits and the thread terminates
 *      The kernel reclaims it automatically because it was detached.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
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
 * ============================================================================= */
void *expiry_thread_func(void *arg)
{
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
     */
    while (!shutdown_flag) {


        sleep(1);

        /*
         * Re-check shutdown_flag after waking — the server may have shut down
         * while we were sleeping. Avoids locking a mutex that may be destroyed.
         */
        if (shutdown_flag)
            break;

        
        // CRITICAL SECTION
        pthread_mutex_lock(&db_mutex);


        ht_purge_expired(db);

        pthread_mutex_unlock(&db_mutex);
        // END CRITICAL SECTION 

    }

    printf("[EXPIRY] Shutdown flag detected. Expiry thread exiting cleanly.\n");

    return NULL;
}
