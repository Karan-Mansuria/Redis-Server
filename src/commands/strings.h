/*
 * =============================================================================
 * strings.h — String operation commands header
 * Location: mini-redis/commands/strings.h
 * =============================================================================
 */

#ifndef STRINGS_H
#define STRINGS_H

#include <stddef.h>
#include "../auth.h"
#include "dispatcher.h"

// Executes the INCR command (Requires WRITER or ADMIN role)
void cmd_incr(ClientSession *session, ParsedCommand *cmd,
              char *response_buf, size_t response_size);

// Executes the APPEND command (Requires WRITER or ADMIN role)
void cmd_append(ClientSession *session, ParsedCommand *cmd,
                char *response_buf, size_t response_size);

#endif // STRINGS_H