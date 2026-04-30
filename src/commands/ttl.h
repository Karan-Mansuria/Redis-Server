/*
 * =============================================================================
 * ttl.h — TTL/expiry commands header
 * Location: mini-redis/commands/ttl.h
 * =============================================================================
 */

#ifndef TTL_H
#define TTL_H

#include <stddef.h>
#include "../auth.h"
#include "dispatcher.h"

// Executes the EXPIRE command (Requires WRITER or ADMIN role)
void cmd_expire(ClientSession *session, ParsedCommand *cmd,
                char *response_buf, size_t response_size);

// Executes the TTL command (Requires READER, WRITER, or ADMIN role)
void cmd_ttl(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size);

#endif // TTL_H