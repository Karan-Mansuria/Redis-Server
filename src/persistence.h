/*
 * =============================================================================
 * persistence.h — Persistence layer interface (SAVE, BGSAVE, load_snapshot)
 * Location: mini-redis/src/persistence.h
 * =============================================================================
 *
 * This header is included by:
 *   - server.c      (calls save_snapshot at shutdown, load_snapshot at startup)
 *   - commands/dispatcher.c (calls save_snapshot for SAVE, bgsave_async for BGSAVE)
 *   - persistence.c (implements these functions)
 *
 * OS CONCEPTS:
 *   Concept 2 — File Locking  : save_snapshot and load_snapshot use flock()
 *   Concept 6 — IPC (fork+pipe): bgsave_async forks a child process
 * =============================================================================
 */

#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stddef.h>     /* size_t */
#include "hashtable.h"  /* HashTable */

/*
 * save_snapshot — Synchronous snapshot to disk with exclusive file lock.
 *
 * Writes all current key-value-expiry entries to `filepath` in text format:
 *   # Mini-Redis snapshot — 2026-04-30 12:00:00
 *   key1 value1 0
 *   key2 value2 1719000000
 *
 * OS Concept 2: Acquires flock(LOCK_EX) before writing. Blocks if another
 * writer (e.g., a concurrent BGSAVE child) holds the lock.
 *
 * IMPORTANT: The caller MUST hold db_mutex before calling this function
 * to guarantee a consistent snapshot of the hash table.
 *
 * Returns: number of keys written, or -1 on error.
 */
int save_snapshot(HashTable *ht, const char *filepath);

/*
 * load_snapshot — Read keys from dump.rdb into the hash table at startup.
 *
 * Called once during server initialization, before any client threads exist.
 * Parses each line, skips expired keys and comment lines, and calls ht_set()
 * and ht_set_expiry() to restore state.
 *
 * OS Concept 2: Acquires flock(LOCK_SH) before reading. Multiple readers
 * can hold LOCK_SH simultaneously; blocks only if a writer holds LOCK_EX.
 *
 * Returns: number of keys loaded, 0 if file not found, -1 on error.
 */
int load_snapshot(HashTable *ht, const char *filepath);

/*
 * bgsave_async — Non-blocking background save via fork() + pipe() (IPC).
 *
 * Flow:
 *   1. Creates a pipe (pipefd[0]=read, pipefd[1]=write)
 *   2. Locks db_mutex and calls fork()
 *   3. CHILD:  saves db to disk, writes "DONE:N" to pipe, exit(0)
 *   4. PARENT: unlocks mutex, spawns a watcher pthread on pipefd[0],
 *              writes "+OK Background saving started" into response_buf,
 *              returns IMMEDIATELY without blocking.
 *
 * OS Concept 6: Inter-Process Communication (IPC) via fork + pipe.
 * The child process is a separate OS process. The pipe is the only
 * communication channel between parent and child.
 *
 * response_buf / response_size: Redis-protocol response written here.
 */
void bgsave_async(HashTable *ht, char *response_buf, size_t response_size);

#endif /* PERSISTENCE_H */
