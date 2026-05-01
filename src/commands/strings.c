
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strings.h"
#include "../hashtable.h"
#include "../auth.h"
#include "../globals.h"

// =============================================================================
// cmd_incr — INCR key
//
// Atomically reads the current value, increments it, writes it back.
// The entire read-modify-write sequence is inside a single mutex lock,
// making it impossible for another thread to interleave.
// =============================================================================
void cmd_incr(ClientSession *session, ParsedCommand *cmd,
              char *response_buf, size_t response_size) {

    if (!has_permission(session, WRITER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied - INCR requires WRITER or ADMIN role\r\n");
        return;
    }

    const char *key = cmd->args[1];

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    // The lock wraps ALL THREE steps: read, modify, write.
    // Releasing the lock between any two steps would create a race condition.
    pthread_mutex_lock(&db_mutex);

    // Step 1: Read the current value
    char *current_val = ht_get(db, key);

    // Step 2: Parse as integer. If key doesn't exist, start at 0.
    long long num = 0;
    if (current_val != NULL) {
        char *end;
        num = strtoll(current_val, &end, 10);

        // strtoll sets end to point past the last valid digit.
        // If *end is not '\0', the value isn't a clean integer.
        if (*end != '\0') {
            pthread_mutex_unlock(&db_mutex);
            snprintf(response_buf, response_size,
                     "-ERR value at '%s' is not an integer\r\n", key);
            return;
        }
    }

    // Step 3: Increment
    num++;

    // Step 4: Write back as string
    char new_val[32];
    snprintf(new_val, sizeof(new_val), "%lld", num);

    int result = ht_set(db, key, new_val);

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    if (result != 0) {
        snprintf(response_buf, response_size, "-ERR out of memory\r\n");
        return;
    }

    // Return the new value as a human-readable integer reply
    snprintf(response_buf, response_size, "Result: (integer) %lld\r\n", num);
}

// =============================================================================
// cmd_append — APPEND key value
// Appends a string to the existing value at key.
// If key doesn't exist, it's created with just the appended value (like SET).
// Returns the new total length.
// =============================================================================
void cmd_append(ClientSession *session, ParsedCommand *cmd,
                char *response_buf, size_t response_size) {

    if (!has_permission(session, WRITER)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied — APPEND requires WRITER or ADMIN role\r\n");
        return;
    }

    const char *key      = cmd->args[1];
    const char *to_append = cmd->args[2];

    // ── CRITICAL SECTION START ────────────────────────────────────────────────
    pthread_mutex_lock(&db_mutex);

    char *existing = ht_get(db, key);

    char new_val[8192];  // 8KB max value
    
    if (existing == NULL) {
        // Key doesn't exist - APPEND behaves like SET
        strncpy(new_val, to_append, sizeof(new_val) - 1);
        new_val[sizeof(new_val) - 1] = '\0';
    } else {
        // Concatenate existing + to_append
        size_t existing_len = strlen(existing);
        size_t append_len   = strlen(to_append);

        if (existing_len + append_len >= sizeof(new_val)) {
            pthread_mutex_unlock(&db_mutex);
            snprintf(response_buf, response_size,
                     "-ERR resulting value too large (max 8192 bytes)\r\n");
            return;
        }

        // Build the concatenated string
        strncpy(new_val, existing, sizeof(new_val) - 1);
        strncat(new_val, to_append, sizeof(new_val) - existing_len - 1);
    }

    int result = ht_set(db, key, new_val);
    size_t new_len = strlen(new_val);

    pthread_mutex_unlock(&db_mutex);
    // ── CRITICAL SECTION END ──────────────────────────────────────────────────

    if (result != 0) {
        snprintf(response_buf, response_size, "-ERR out of memory\r\n");
        return;
    }

    // Return new length as human-readable reply
    snprintf(response_buf, response_size, "Result: New length is (integer) %zu\r\n", new_len);
}