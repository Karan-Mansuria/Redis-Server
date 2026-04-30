/*
 * =============================================================================
 * commands.h — Umbrella header for the commands subsystem
 * Location: mini-redis/src/commands.h
 * =============================================================================
 *
 * server.c includes this single header to get access to dispatch_command(),
 * which is the only function it needs from the commands/ subsystem.
 *
 * This is a facade pattern: the internal structure of commands/
 * (dispatcher.c, keyval.c, strings.c, ttl.c) is hidden from server.c.
 * server.c only knows about dispatch_command() — it does not care how
 * commands are routed or which handler implements each command.
 *
 * Without this file, server.c would have to include
 * "commands/dispatcher.h" directly, creating a tight coupling between
 * the server and the commands directory structure.
 * =============================================================================
 */

#ifndef COMMANDS_H
#define COMMANDS_H

/* Re-export the dispatcher interface — this is all server.c needs */
#include "commands/dispatcher.h"

#endif /* COMMANDS_H */
