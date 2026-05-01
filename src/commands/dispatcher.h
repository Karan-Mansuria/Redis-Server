/*
 * dispatcher.h — Command parser and router header
 * This header defines the structures and functions for parsing raw client
 * input into distinct tokens and routing them to the appropriate command
 * handler based on role permissions.
 */

#ifndef DISPATCHER_H
#define DISPATCHER_H

#include <stddef.h>  // Required for size_t
#include "../auth.h" // Required for the ClientSession struct

#define MAX_ARGS 10

#define MAX_ARG_LEN 128


typedef struct {
    int argc;
    char args[MAX_ARGS][MAX_ARG_LEN];
} ParsedCommand;

/*
 * Splits a raw input string into distinct tokens based on spaces/tabs.
 * Populates the provided ParsedCommand struct safely without modifying
 * the original input string.
 * Returns the number of arguments parsed (argc).
 */
int parse_command(const char *raw_input, ParsedCommand *cmd);

/*
 * The main switchboard for the Redis server. Parses the command, 
 * checks the AUTH gate, verifies role permissions, and routes the 
 * execution to the proper handler function.
 * Returns 0 on normal execution, or -1 if the client issued a QUIT command.
 */
int dispatch_command(ClientSession *session,
                     const char    *raw_input,
                     char          *response_buf,
                     size_t         response_size);

#endif // DISPATCHER_H