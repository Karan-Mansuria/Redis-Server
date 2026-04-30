// =============================================================================
// commands/ttl.c — TTL/expiry commands: EXPIRE, TTL
// Location: mini-redis/commands/ttl.c
// =============================================================================
//
// HOW TTL WORKS IN THIS PROJECT (three-part system):
//
//   Part 1 — EXPIRE command (this file):
//     Stores a Unix timestamp (time(NULL) + seconds) in the Entry struct
//     alongside the key's value. This is the expiry_time field in hashtable.h.
//
//   Part 2 — Lazy expiry check in GET/EXISTS (keyval.c):
//     When a client reads a key, we check if expiry_time has passed.
//     If yes, we return nil/$-1 immediately even if the key still physically
//     exists in the table. Real Redis calls this "lazy expiration".
//
//   Part 3 — Active expiry sweep (expiry.c background thread):
//     Every second, the expiry thread locks db_mutex and calls
//     ht_purge_expired() to physically remove all expired entries.
//     This reclaims memory. Real Redis calls this "active expiration".
//
// This two-pronged approach (lazy + active) is exactly what production Redis does.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ttl.h"
#include "../hashtable.h"
#include "../auth.h"
#include "../globals.h"

// =============================================================================
// cmd_expire — EXPIRE key seconds
//
// Sets the expiry_time field of the Entry for `key` to time(NULL) + seconds.
// After that timestamp passes, the key is treated as deleted.
//
// OS Concept 1: WRITER or above required to set expiry
// OS Concept 3: Mutex ensures the expiry_time is set atomically
// =============================================================================
void cmd_expire(ClientSession *session, ParsedCommand *cmd,
                char *response_buf, size_t response_size) {

    if (!has_permission(session, WRITER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied — EXPIRE requires WRITER or ADMIN role\r\n");
        return;
    }

    const char *key      = cmd->args[1];
    const char *sec_str  = cmd->args[2];

    // Parse the seconds argument
    char *end;
    long seconds = strtol(sec_str, &end, 10);

    if (*end != '\0' || seconds <= 0) {
        snprintf(response_buf, response_size,
                 "-ERR invalid expire time — must be a positive integer\r\n");
        return;
    }

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    pthread_mutex_lock(&db_mutex);

    // First check if the key exists at all
    if (!ht_exists(db, key)) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(response_buf, response_size, "Failed: Key not found.\r\n");
        return;
    }

    // Calculate the Unix timestamp when the key should expire
    time_t expiry_timestamp = time(NULL) + seconds;
    ht_set_expiry(db, key, expiry_timestamp);

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    // Timeout successfully set
    snprintf(response_buf, response_size, "Success: Timeout set.\r\n");
}

// =============================================================================
// cmd_ttl — TTL key
//
// Returns the remaining seconds before the key expires.
// Three possible outcomes mirror real Redis exactly:
//   :N    where N > 0  — key exists and has N seconds remaining
//   :-1               — key exists but has no expiry (will live forever)
//   :-2               — key does not exist
//
// OS Concept 1: READER role sufficient — TTL is a read-only operation
// OS Concept 3: Mutex prevents race where key expires while we're reading TTL
// =============================================================================
void cmd_ttl(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size) {

    if (!has_permission(session, READER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied\r\n");
        return;
    }

    const char *key = cmd->args[1];

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    pthread_mutex_lock(&db_mutex);

    if (!ht_exists(db, key)) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(response_buf, response_size, "Result: -2 (Key does not exist)\r\n");
        return;
    }

    time_t expiry = ht_get_expiry(db, key);

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    if (expiry == 0) {
        // Key exists but has no expiry — lives forever
        snprintf(response_buf, response_size, "Result: -1 (Key lives forever)\r\n");
    } else {
        long remaining = (long)(expiry - time(NULL));
        if (remaining < 0) remaining = 0;
        snprintf(response_buf, response_size, "Result: %ld seconds remaining\r\n", remaining);
    }
}