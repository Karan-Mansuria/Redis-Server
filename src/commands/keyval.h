#ifndef KEYVAL_H
#define KEYVAL_H

#include <stddef.h>
#include "../auth.h"
#include "dispatcher.h"

// Executes the SET command (Requires WRITER or ADMIN role)
void cmd_set(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size);

// Executes the GET command (Requires READER, WRITER, or ADMIN role)
void cmd_get(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size);

// Executes the DEL command (Requires ADMIN role)
void cmd_del(ClientSession *session, ParsedCommand *cmd,
             char *response_buf, size_t response_size);

// Executes the EXISTS command (Requires READER, WRITER, or ADMIN role)
void cmd_exists(ClientSession *session, ParsedCommand *cmd,
                char *response_buf, size_t response_size);

#endif // KEYVAL_H