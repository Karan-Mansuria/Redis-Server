// =============================================================================
// commands/dispatcher.c — Command parser and router
// Location: mini-redis/commands/dispatcher.c
// =============================================================================
//
// This file is the brain of the command system. It does three things:
//   1. PARSE  — split raw text input into tokens (command + arguments)
//   2. GUARD  — reject unauthenticated clients and wrong argument counts
//   3. ROUTE  — call the correct handler function in keyval.c, strings.c, ttl.c
//               or persistence.c
//
// Real Redis has a command table (an array of structs) where each entry
// holds the command name, handler pointer, min/max arg count, and flags.
// We use a simpler if-else chain here which is easier to read and debug,
// while demonstrating the same architectural pattern.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dispatcher.h"
#include "keyval.h"
#include "strings.h"
#include "ttl.h"
#include "../auth.h"
#include "../persistence.h"
#include "../globals.h"

// -----------------------------------------------------------------------------
// INTERNAL HELPER: to_uppercase
// Converts a string to uppercase IN PLACE so commands are case-insensitive.
// "set", "Set", "SET" all work the same way, matching real Redis behaviour.
// -----------------------------------------------------------------------------
static void to_uppercase(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = (char)toupper((unsigned char)str[i]);
    }
}

// -----------------------------------------------------------------------------
// INTERNAL HELPER: bad_args
// Writes a standardised wrong-argument-count error into response_buf.
// Usage: bad_args("SET", "SET key value", response_buf, response_size)
// -----------------------------------------------------------------------------
static void bad_args(const char *cmd_name,
                     const char *usage,
                     char       *response_buf,
                     size_t      response_size) {
    snprintf(response_buf, response_size,
             "-ERR wrong number of arguments for '%s' (usage: %s)\r\n",
             cmd_name, usage);
}

// =============================================================================
// parse_command
//
// Tokenizes raw_input by splitting on spaces and tabs.
// Stores each token in cmd->args[][] and sets cmd->argc.
//
// strtok() is destructive (replaces delimiters with '\0'), so we work on
// a local copy of raw_input. This keeps raw_input intact for logging.
// =============================================================================
int parse_command(const char *raw_input, ParsedCommand *cmd) {
    cmd->argc = 0;

    // Work on a local copy — strtok modifies the string it receives
    char copy[MAX_ARGS * MAX_ARG_LEN];
    strncpy(copy, raw_input, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *token = strtok(copy, " \t");
    while (token != NULL && cmd->argc < MAX_ARGS) {
        strncpy(cmd->args[cmd->argc], token, MAX_ARG_LEN - 1);
        cmd->args[cmd->argc][MAX_ARG_LEN - 1] = '\0';
        cmd->argc++;
        token = strtok(NULL, " \t");
    }

    return cmd->argc;
}

// =============================================================================
// dispatch_command
//
// Called from client_handler() in server.c for every line the client sends.
//
// Flow:
//   parse → uppercase → check AUTH gate → route to handler → return result
//
// Returns 0 normally, -1 if the client sent QUIT (signals caller to close conn).
// =============================================================================
int dispatch_command(ClientSession *session,
                     const char    *raw_input,
                     char          *response_buf,
                     size_t         response_size) {

    // Zero-out the response buffer so stale data never leaks
    memset(response_buf, 0, response_size);

    // Parse the raw input into tokens
    ParsedCommand cmd;
    if (parse_command(raw_input, &cmd) == 0) {
        // Empty input after stripping whitespace — send nothing
        snprintf(response_buf, response_size, "+\r\n");
        return 0;
    }

    // Uppercase the command name for case-insensitive matching.
    // args[1], args[2] (keys and values) stay as-is — keys are case-sensitive.
    to_uppercase(cmd.args[0]);

    // =========================================================================
    // AUTH GATE
    //
    // AUTH is the only command allowed before authentication.
    // Every other command checks session->authenticated first.
    //
    // OS Concept 1: Role-Based Authorization
    // =========================================================================

    // ── Handle AUTH separately — it's allowed before authentication ──────────
    if (strcmp(cmd.args[0], "AUTH") == 0) {
        if (cmd.argc != 3) {
            bad_args("AUTH", "AUTH username password", response_buf, response_size);
            return 0;
        }
        // authenticate() is in auth.c — sets session->role and session->username
        if (authenticate(session, cmd.args[1], cmd.args[2])) {
            snprintf(response_buf, response_size, "+OK authenticated as %s\r\n",
                     session->username);
        } else {
            snprintf(response_buf, response_size,
                     "-ERR invalid username or password\r\n");
        }
        return 0;
    }

    // ── PING — allowed before auth, useful for connection testing ────────────
    if (strcmp(cmd.args[0], "PING") == 0) {
        snprintf(response_buf, response_size, "+PONG\r\n");
        return 0;
    }

    // ── QUIT — client wants to disconnect cleanly ─────────────────────────────
    if (strcmp(cmd.args[0], "QUIT") == 0) {
        snprintf(response_buf, response_size, "+OK bye\r\n");
        return -1;  // Signal caller to close the connection
    }

    // ── All other commands require authentication ─────────────────────────────
    if (!session->authenticated) {
        snprintf(response_buf, response_size,
                 "-ERR not authenticated — send AUTH username password first\r\n");
        return 0;
    }

    // =========================================================================
    // COMMAND ROUTING
    //
    // Each handler is in a dedicated file:
    //   keyval.c   → SET, GET, DEL, EXISTS
    //   strings.c  → INCR, APPEND
    //   ttl.c      → EXPIRE, TTL
    //   persistence.c → SAVE, BGSAVE (routed directly, no wrapper needed)
    //
    // Every handler receives:
    //   session  — for permission checks (has_permission())
    //   &cmd     — the parsed tokens
    //   response_buf, response_size — where to write the response
    // =========================================================================

    // ── Tier 1: Core key-value commands (keyval.c) ───────────────────────────

    if (strcmp(cmd.args[0], "SET") == 0) {
        if (cmd.argc < 3) {
            bad_args("SET", "SET key value", response_buf, response_size);
            return 0;
        }
        cmd_set(session, &cmd, response_buf, response_size);
    }

    else if (strcmp(cmd.args[0], "GET") == 0) {
        if (cmd.argc != 2) {
            bad_args("GET", "GET key", response_buf, response_size);
            return 0;
        }
        cmd_get(session, &cmd, response_buf, response_size);
    }

    else if (strcmp(cmd.args[0], "DEL") == 0) {
        if (cmd.argc != 2) {
            bad_args("DEL", "DEL key", response_buf, response_size);
            return 0;
        }
        cmd_del(session, &cmd, response_buf, response_size);
    }

    else if (strcmp(cmd.args[0], "EXISTS") == 0) {
        if (cmd.argc != 2) {
            bad_args("EXISTS", "EXISTS key", response_buf, response_size);
            return 0;
        }
        cmd_exists(session, &cmd, response_buf, response_size);
    }

    // ── Tier 2a: String modifier commands (strings.c) ────────────────────────

    else if (strcmp(cmd.args[0], "INCR") == 0) {
        if (cmd.argc != 2) {
            bad_args("INCR", "INCR key", response_buf, response_size);
            return 0;
        }
        cmd_incr(session, &cmd, response_buf, response_size);
    }

    else if (strcmp(cmd.args[0], "APPEND") == 0) {
        if (cmd.argc != 3) {
            bad_args("APPEND", "APPEND key value", response_buf, response_size);
            return 0;
        }
        cmd_append(session, &cmd, response_buf, response_size);
    }

    // ── Tier 2b: TTL/expiry commands (ttl.c) ─────────────────────────────────

    else if (strcmp(cmd.args[0], "EXPIRE") == 0) {
        if (cmd.argc != 3) {
            bad_args("EXPIRE", "EXPIRE key seconds", response_buf, response_size);
            return 0;
        }
        cmd_expire(session, &cmd, response_buf, response_size);
    }

    else if (strcmp(cmd.args[0], "TTL") == 0) {
        if (cmd.argc != 2) {
            bad_args("TTL", "TTL key", response_buf, response_size);
            return 0;
        }
        cmd_ttl(session, &cmd, response_buf, response_size);
    }

    // ── Tier 3: Persistence commands (routed directly to persistence.c) ───────
    //
    // SAVE and BGSAVE live in persistence.c, not in a commands/ file.
    // The dispatcher calls them directly — no wrapper needed.
    // This is intentional: persistence is a server-level concern, not a
    // command-category concern.

    else if (strcmp(cmd.args[0], "SAVE") == 0) {
        // OS Concept 1: Only ADMIN can trigger a manual save
        if (!has_permission(session, ADMIN)) {
            snprintf(response_buf, response_size,
                     "-ERR permission denied — SAVE requires ADMIN role\r\n");
            return 0;
        }
        // Lock db before reading all entries to write to disk
        // OS Concept 3 + 4: Mutex for safe read under concurrency
        pthread_mutex_lock(&db_mutex);
        int saved = save_snapshot(db, DUMP_FILE);
        pthread_mutex_unlock(&db_mutex);

        if (saved >= 0) {
            snprintf(response_buf, response_size, "+OK saved %d keys\r\n", saved);
        } else {
            snprintf(response_buf, response_size, "-ERR save failed\r\n");
        }
    }

    else if (strcmp(cmd.args[0], "BGSAVE") == 0) {
        // OS Concept 1: Only ADMIN can trigger background save
        if (!has_permission(session, ADMIN)) {
            snprintf(response_buf, response_size,
                     "-ERR permission denied — BGSAVE requires ADMIN role\r\n");
            return 0;
        }
        // bgsave_async() is in persistence.c
        // It fork()s a child, which saves to disk and notifies parent via pipe
        // OS Concept 6: IPC via fork + pipe
        bgsave_async(db, response_buf, response_size);
    }

    // ── UNKNOWN COMMAND ───────────────────────────────────────────────────────

    else {
        snprintf(response_buf, response_size,
                 "-ERR unknown command '%s'. "
                 "Available: AUTH, SET, GET, DEL, EXISTS, INCR, APPEND, "
                 "EXPIRE, TTL, SAVE, BGSAVE, PING, QUIT\r\n",
                 cmd.args[0]);
    }

    return 0;
}