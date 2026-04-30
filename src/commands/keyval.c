// =============================================================================
// commands/keyval.c — Core key-value commands: SET, GET, DEL, EXISTS
// Location: mini-redis/commands/keyval.c
// =============================================================================
//
// OS CONCEPTS DEMONSTRATED IN THIS FILE:
//   Concept 1 — Role-Based Authorization : has_permission() before every write
//   Concept 3 — Concurrency Control      : pthread_mutex_lock/unlock on db
//   Concept 4 — Data Consistency         : mutex prevents dirty reads + lost updates
//
// RESPONSE FORMAT (simplified Redis protocol):
//   +OK\r\n              — simple success string
//   -ERR message\r\n     — error
//   $5\r\nAlice\r\n      — bulk string (length prefix then value)
//   $-1\r\n              — nil / key not found
//   :1\r\n               — integer reply
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "keyval.h"
#include "../hashtable.h"
#include "../auth.h"
#include "../globals.h"

// =============================================================================
// cmd_set — SET key value
//
// Stores a key-value pair in the hash table.
// If the key already exists, its value is overwritten.
// Expiry time is NOT reset by SET — if the key had a TTL, it keeps it.
// This matches real Redis behaviour.
//
// OS Concept 1: Requires WRITER role or above
// OS Concept 3: Locks db_mutex before writing to db
// OS Concept 4: Mutex prevents another thread from reading a half-written value
// =============================================================================
void cmd_set(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size) {

    // Permission check — READERs cannot store data
    // ADMIN(0) and WRITER(1) can. has_permission checks session->role <= WRITER.
    if (!has_permission(session, WRITER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied — SET requires WRITER or ADMIN role\r\n");
        return;
    }

    const char *key   = cmd->args[1];
    const char *value = cmd->args[2];

    // Validate key length — prevent absurdly large keys
    if (strlen(key) > 256) {
        snprintf(response_buf, response_size,
                 "-ERR key too long (max 256 characters)\r\n");
        return;
    }

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    // OS Concept 3 + 4: Lock the mutex before touching the shared hash table.
    // Without this lock, two threads doing SET simultaneously could corrupt
    // the linked list inside the hash table bucket.
    pthread_mutex_lock(&db_mutex);

    int result = ht_set(db, key, value);

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    if (result == 0) {
        snprintf(response_buf, response_size, "Success: Value stored.\r\n");
    } else {
        snprintf(response_buf, response_size, "-ERR out of memory\r\n");
    }
}

// =============================================================================
// cmd_get — GET key
//
// Returns the value stored at key as a bulk string.
// Returns $-1 (nil) if the key does not exist.
// Automatically skips keys whose expiry_time has passed — the expiry thread
// will clean them up asynchronously, but GET must not return stale data.
//
// OS Concept 1: READER role sufficient — all authenticated users can GET
// OS Concept 3: Locks db_mutex even for reads (prevents dirty reads)
// OS Concept 4: A reader must never see a value that is mid-write by another thread
// =============================================================================
void cmd_get(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size) {

    // All authenticated roles (ADMIN, WRITER, READER) can GET
    if (!has_permission(session, READER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied\r\n");
        return;
    }

    const char *key = cmd->args[1];

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    pthread_mutex_lock(&db_mutex);

    // Check expiry before returning the value.
    // Even if the key exists in the hash table, if its TTL has passed we treat
    // it as non-existent. The background expiry thread removes it eventually.
    time_t expiry = ht_get_expiry(db, key);
    if (expiry != 0 && expiry <= time(NULL)) {
        // Key has expired — treat as missing
        pthread_mutex_unlock(&db_mutex);
        snprintf(response_buf, response_size, "(nil) - Key not found or expired.\r\n");
        return;
    }

    char *value = ht_get(db, key);

    if (value == NULL) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(response_buf, response_size, "(nil) - Key not found or expired.\r\n");
        return;
    }

    // Copy value before releasing lock — another thread could DEL this key
    // the moment we unlock if we don't copy first
    char value_copy[4096];
    strncpy(value_copy, value, sizeof(value_copy) - 1);
    value_copy[sizeof(value_copy) - 1] = '\0';

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    snprintf(response_buf, response_size, "Value: %s\r\n", value_copy);
}

// =============================================================================
// cmd_del — DEL key
//
// Deletes a key from the hash table.
// Returns :1 if the key existed and was deleted, :0 if key was not found.
// Restricted to ADMIN only — deletion is a destructive irreversible operation.
//
// OS Concept 1: ADMIN role required — most restrictive permission
// OS Concept 3: Mutex prevents deleting a key another thread is mid-read on
// OS Concept 4: No thread should read a key that is being deleted simultaneously
// =============================================================================
void cmd_del(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size) {

    // Deletion is admin-only — writers can SET but cannot destroy data
    if (!has_permission(session, ADMIN)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied — DEL requires ADMIN role\r\n");
        return;
    }

    const char *key = cmd->args[1];

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    pthread_mutex_lock(&db_mutex);

    int deleted = ht_delete(db, key);
    // ht_delete returns 1 if found and deleted, 0 if key did not exist

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    if (deleted) {
        snprintf(response_buf, response_size, "Success: Key deleted.\r\n");
    } else {
        snprintf(response_buf, response_size, "Failed: Key not found.\r\n");
    }
}

// =============================================================================
// cmd_exists — EXISTS key
//
// Returns :1 if the key exists and has not expired, :0 otherwise.
// This is a pure read — no modification to the database.
//
// OS Concept 1: READER role sufficient
// OS Concept 3: Still locks mutex — a read without a lock could see a key
//               that is being deleted by another thread mid-operation
// =============================================================================
void cmd_exists(ClientSession *session, ParsedCommand *cmd,
                char *response_buf, size_t response_size) {

    if (!has_permission(session, READER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied\r\n");
        return;
    }

    const char *key = cmd->args[1];

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    pthread_mutex_lock(&db_mutex);

    // Check expiry first — an expired key does not "exist" from the client's view
    time_t expiry = ht_get_expiry(db, key);
    if (expiry != 0 && expiry <= time(NULL)) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(response_buf, response_size, "Result: 0 (Key does not exist)\r\n");
        return;
    }

    int exists = ht_exists(db, key);

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    if (exists) {
        snprintf(response_buf, response_size, "Result: 1 (Key exists)\r\n");
    } else {
        snprintf(response_buf, response_size, "Result: 0 (Key does not exist)\r\n");
    }
}