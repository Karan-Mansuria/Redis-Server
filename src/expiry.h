/*
 * Concept 3 — Concurrency Control
 * expiry_thread_func runs as a detached pthread alongside all client
 * threads. It locks db_mutex before scanning, ensuring it never races
 * with a concurrent SET or GET.
 *
 * HOW EXPIRY WORKS:
 *   1. Client sends: EXPIRE counter 60
 *   2. cmd_expire() calls ht_set_expiry(db, "counter", time(NULL)+60)
 *   3. Entry's expiry_time is now set to a Unix timestamp 60 seconds in future
 *   4. This thread wakes up every second and calls ht_purge_expired(db)
 *   5. ht_purge_expired scans all 256 buckets and frees any entry where:
 *        entry->expiry_time != 0 && entry->expiry_time <= time(NULL)
 *   6. Key is silently deleted — next GET returns $-1 (not found)
 *
 * WHY A BACKGROUND THREAD:
 *   Redis actually uses BOTH strategies. Lazy deletion checks expiry on GET.
 *   Active expiry (this thread) ensures memory is freed even for keys that
 *   are never accessed again. 
 */

#ifndef EXPIRY_H
#define EXPIRY_H


void *expiry_thread_func(void *arg);

#endif /* EXPIRY_H */
