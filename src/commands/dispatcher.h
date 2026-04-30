/*
 * =============================================================================
 * dispatcher.h — Command parser and router header
 * Location: mini-redis/commands/dispatcher.h
 * =============================================================================
 * * This header defines the structures and functions for parsing raw client
 * input into distinct tokens and routing them to the appropriate command
 * handler based on role permissions.
 * =============================================================================
 */

#ifndef DISPATCHER_H
#define DISPATCHER_H

#include <stddef.h>  // Required for size_t
#include "../auth.h" // Required for the ClientSession struct

// Maximum number of arguments a single command can have (e.g., SET key value = 3)
#define MAX_ARGS 10

// Maximum character length for a single argument (like a key or value)
#define MAX_ARG_LEN 128

/*
 * ParsedCommand Struct
 * * Holds the tokenized version of the client's raw input string.
 * argc: The number of arguments successfully parsed.
 * args: A 2D array holding the string tokens (args[0] is the command name).
 */
typedef struct {
    int argc;
    char args[MAX_ARGS][MAX_ARG_LEN];
} ParsedCommand;

/*
 * parse_command
 * * Splits a raw input string into distinct tokens based on spaces/tabs.
 * Populates the provided ParsedCommand struct safely without modifying
 * the original input string.
 * Returns the number of arguments parsed (argc).
 */
int parse_command(const char *raw_input, ParsedCommand *cmd);

/*
 * dispatch_command
 * * The main switchboard for the Redis server. Parses the command, 
 * checks the AUTH gate, verifies role permissions, and routes the 
 * execution to the proper handler function.
 * * Returns 0 on normal execution, or -1 if the client issued a QUIT command.
 */
int dispatch_command(ClientSession *session,
                     const char    *raw_input,
                     char          *response_buf,
                     size_t         response_size);

#endif // DISPATCHER_H