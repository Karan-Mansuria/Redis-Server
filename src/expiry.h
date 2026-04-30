/*
 * =============================================================================
 * expiry.h — Background TTL expiry thread interface
 * Location: mini-redis/src/expiry.h
 * =============================================================================
 *
 * OS CONCEPT DEMONSTRATED:
 *   Concept 3 — Concurrency Control
 *     expiry_thread_func runs as a detached pthread alongside all client
 *     threads. It locks db_mutex before scanning, ensuring it never races
 *     with a concurrent SET or GET. This is OS-level thread scheduling in
 *     action — the OS decides when this thread runs, not the programmer.
 *
 * HOW EXPIRY WORKS (end-to-end):
 *   1. Client sends: EXPIRE counter 60
 *   2. cmd_expire() (ttl.c) calls ht_set_expiry(db, "counter", time(NULL)+60)
 *   3. Entry's expiry_time is now set to a Unix timestamp 60 seconds in future
 *   4. This thread wakes up every second and calls ht_purge_expired(db)
 *   5. ht_purge_expired scans all 256 buckets and frees any entry where:
 *        entry->expiry_time != 0 && entry->expiry_time <= time(NULL)
 *   6. Key is silently deleted — next GET returns $-1 (not found)
 *
 * WHY A BACKGROUND THREAD (not lazy deletion)?
 *   Redis actually uses BOTH strategies. Lazy deletion checks expiry on GET.
 *   Active expiry (this thread) ensures memory is freed even for keys that
 *   are never accessed again. We implement the active strategy here because
 *   it demonstrates OS threading concepts more clearly.
 * =============================================================================
 */

#ifndef EXPIRY_H
#define EXPIRY_H

/*
 * expiry_thread_func — entry point for the background expiry thread.
 *
 * Signature matches pthread_create's expected function pointer:
 *   void *(*start_routine)(void *)
 *
 * arg: unused (NULL is passed from server.c)
 * Returns: NULL always (thread exit)
 *
 * Loop behaviour:
 *   while (!shutdown_flag) {
 *       sleep(1);                       // Check every 1 second
 *       pthread_mutex_lock(&db_mutex);  // Prevent races with client threads
 *       ht_purge_expired(db);           // Delete all keys past their TTL
 *       pthread_mutex_unlock(&db_mutex);
 *   }
 */
void *expiry_thread_func(void *arg);

#endif /* EXPIRY_H */
