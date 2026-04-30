// =============================================================================
// globals.h — Shared global variable declarations
// Location: mini-redis/globals.h
// =============================================================================
//
// WHY THIS FILE EXISTS:
// db, db_mutex, and shutdown_flag are defined ONCE in server.c.
// Every other file that needs them uses `extern` to say "this variable
// exists somewhere else — find it at link time."
// Without this file, every .c that needs db would have to write its own
// extern declarations, which is error-prone and messy.
//
// RULE: This file only has extern declarations and #defines.
//       Never define (allocate) variables here — only declare them.
// =============================================================================

#ifndef GLOBALS_H
#define GLOBALS_H

#include <pthread.h>
#include <signal.h>
#include "hashtable.h"

// The shared in-memory key-value store.
// Defined in server.c. Every access MUST be wrapped in db_mutex.
extern HashTable *db;

// The mutex that protects db.
// Lock before ANY read or write to db. Unlock immediately after.
// This is OS Concept 3 (Concurrency) and OS Concept 4 (Data Consistency).
extern pthread_mutex_t db_mutex;

// Set to 1 by SIGINT handler. All loops check this to exit cleanly.
extern volatile sig_atomic_t shutdown_flag;

// Path constants — defined here so every file uses the same strings
#define DUMP_FILE  "dump.rdb"
#define LOG_FILE   "server.log"

#endif // GLOBALS_H