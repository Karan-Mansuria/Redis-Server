#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stddef.h> 
#include "hashtable.h"  

/*
 * save_snapshot — Synchronous snapshot to disk with exclusive file lock.
 
 * IMPORTANT: The caller MUST hold db_mutex before calling this function
 * to guarantee a consistent snapshot of the hash table.
 */
int save_snapshot(HashTable *ht, const char *filepath);

/*
 * load_snapshot — Read keys from dump.rdb into the hash table at startup.
 *
 * Called once during server initialization, before any client threads exist.
 * Parses each line, skips expired keys and comment lines, and calls ht_set()
 * and ht_set_expiry() to restore state.
 */
int load_snapshot(HashTable *ht, const char *filepath);

/*
 * bgsave_async - Non-blocking background save via fork() + pipe() (IPC).
 *
 * Flow:
 *   1. Creates a pipe (pipefd[0]=read, pipefd[1]=write)
 *   2. Locks db_mutex and calls fork()
 *   3. CHILD:  saves db to disk, writes "DONE:N" to pipe, exit(0)
 *   4. PARENT: unlocks mutex, spawns a watcher pthread on pipefd[0],
 *              writes "+OK Background saving started" into response_buf,
 *              returns IMMEDIATELY without blocking.
 */
void bgsave_async(HashTable *ht, char *response_buf, size_t response_size);

#endif /* PERSISTENCE_H */
