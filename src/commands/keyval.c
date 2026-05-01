#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "keyval.h"
#include "../hashtable.h"
#include "../auth.h"
#include "../globals.h"

void cmd_set(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size) {

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


void cmd_del(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size) {

    // Deletion is admin-only - writers can SET but cannot destroy data
    if (!has_permission(session, ADMIN)) {
        snprintf(response_buf, response_size,
                 "-ERR permission denied - DEL requires ADMIN role\r\n");
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