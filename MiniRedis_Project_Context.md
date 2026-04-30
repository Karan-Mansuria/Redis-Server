# Mini Redis Server — Complete Project Context Document
### EGC 301P Operating Systems Lab Mini Project
### Language: C | Domain: Systems Programming

---

## PURPOSE OF THIS DOCUMENT

This document contains the COMPLETE context of a Mini Redis Server project being built for an
Operating Systems Lab course. It is meant to be shared with AI assistants to give full context
before asking for help on any specific component. The project is being built incrementally,
one component at a time. Do NOT generate the full project at once. Only help with the specific
component asked about, while keeping this full context in mind.

---

## WHAT IS THIS PROJECT (Simple Explanation)

A Mini Redis Server is a program that runs continuously in the background acting as a
live dictionary stored in RAM. Multiple client programs can connect to it over a network
(TCP sockets) and store or retrieve key-value pairs simultaneously.

Example:
- Client sends: SET name Alice     → Server stores "name" = "Alice"
- Client sends: GET name           → Server returns "Alice"
- Client sends: DEL name           → Server deletes the key
- Client sends: INCR counter       → Server atomically increments counter by 1

The impressive and OS-relevant part is NOT the data storage itself. It is that 100 clients
can do this SIMULTANEOUSLY without corrupting each other's data. This is where all the OS
concepts (concurrency, locks, sockets, IPC) come in.

Redis is used by Twitter, Instagram, Uber, Swiggy — it is one of the most widely used
backend infrastructure tools in the industry.

---

## PROJECT DIRECTORY STRUCTURE

```
mini-redis/
│
├── src/
│   ├── server.c          ← Main server loop, accepts clients, spawns threads
│   ├── commands.c        ← All command implementations (SET, GET, DEL, etc.)
│   ├── commands.h        ← Command function declarations
│   ├── hashtable.c       ← Custom hash table (built from scratch, NO external library)
│   ├── hashtable.h       ← Hash table struct and function declarations
│   ├── auth.c            ← Role-based authorization logic
│   ├── auth.h            ← Auth struct and function declarations
│   ├── persistence.c     ← SAVE and BGSAVE (disk read/write with file locking)
│   ├── persistence.h     ← Persistence function declarations
│   └── expiry.c          ← Background thread that deletes expired keys (TTL system)
│       expiry.h
│
├── dump.rdb              ← Snapshot file (created at runtime, not pre-existing)
├── server.log            ← Log file (created at runtime)
├── Makefile              ← Build file
└── README.md
```

---

## HASH TABLE (Built From Scratch — No External Library)

---

## ROLE-BASED AUTHORIZATION (Mandatory Concept 1)

### Three Roles:

| Role   | Numeric Value | Allowed Commands                                          |
|--------|---------------|-----------------------------------------------------------|
| ADMIN  | 0             | ALL commands including DEL, SAVE, BGSAVE, FLUSHALL        |
| WRITER | 1             | SET, GET, EXISTS, INCR, APPEND, EXPIRE, TTL               |
| READER | 2             | GET, EXISTS, TTL only. Cannot modify data.                |

### How Auth Works at Runtime:
1. Client connects via TCP socket.
2. Server creates a ClientSession struct for this client's thread.
3. Client MUST send AUTH command before any other command:
   - Format: `AUTH <username> <password>`
   - Example: `AUTH admin admin123`
4. Server checks credentials against hardcoded user table.
5. Server sets session->role accordingly.
6. Every command handler checks session->role before executing.
7. If unauthorized → server sends back: `-ERR Permission denied`

### ClientSession Struct:

```c
typedef enum { ADMIN = 0, WRITER = 1, READER = 2 } Role;

typedef struct {
    int     socket_fd;         // The client's connected socket
    Role    role;              // Assigned after AUTH
    char    username[64];      // Logged in username
    int     authenticated;     // 0 = not yet authenticated, 1 = authenticated
} ClientSession;
```

### Hardcoded Users (for project simplicity):

```c
typedef struct {
    char username[64];
    char password[64];
    Role role;
} User;

User user_table[] = {
    {"admin",  "admin123",  ADMIN},
    {"writer", "writer123", WRITER},
    {"reader", "reader123", READER}
};
int user_count = 3;
```

### Permission Check Function:

```c
int has_permission(ClientSession *session, Role required_role) {
    if (!session->authenticated) return 0;
    return session->role <= required_role;  // ADMIN=0 can do everything
}
```

---

## FILE LOCKING (Mandatory Concept 2)

### Where File Locking Is Used:
File locking is used in the SAVE and BGSAVE commands when writing the snapshot
(dump.rdb) to disk.

### Why It Matters:
Without file locking, if two clients simultaneously send SAVE, both processes write to
dump.rdb at the same time → file gets corrupted with interleaved data.

### Implementation Using flock():

```c
#include <sys/file.h>

void save_snapshot(HashTable *ht) {
    int fd = open("dump.rdb", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return; }

    // Acquire exclusive write lock (blocks if another process holds it)
    flock(fd, LOCK_EX);

    // Now safe to write
    ht_save_to_file(ht, fd);

    // Release lock
    flock(fd, LOCK_UN);
    close(fd);
}

void load_snapshot(HashTable *ht) {
    FILE *f = fopen("dump.rdb", "r");
    if (!f) return;  // No snapshot exists yet — first run

    int fd = fileno(f);

    // Acquire shared read lock
    flock(fd, LOCK_SH);

    ht_load_from_file(ht, f);

    flock(fd, LOCK_UN);
    fclose(f);
}
```

### dump.rdb File Format (Simple Text Format):

```
key1 value1 expiry_timestamp
key2 value2 0
name Alice 0
counter 42 0
session_abc token123 1719000000
```

Each line: `<key> <value> <expiry_time_t>`
expiry_time = 0 means no expiry.

---

## CONCURRENCY CONTROL (Mandatory Concept 3)

### Threading Model:
- Main thread: runs `accept()` loop waiting for new client connections.
- Per-client thread: spawned via `pthread_create()` for every new client.
  Handles all communication with that client until disconnect.
- Expiry thread: one background thread runs continuously, checks for expired
  keys every second and deletes them.

### Global Shared State:

```c
HashTable         *db;                              // The shared in-memory database
pthread_mutex_t    db_mutex = PTHREAD_MUTEX_INITIALIZER;  // Protects db
int                shutdown_flag = 0;               // Set to 1 to stop the server
pthread_mutex_t    shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
```

### Mutex Usage Rule (CRITICAL):
EVERY read or write to the db hash table MUST be wrapped in:
```c
pthread_mutex_lock(&db_mutex);
// ... access db here ...
pthread_mutex_unlock(&db_mutex);
```
This prevents race conditions when two threads access db simultaneously.

### Semaphore Usage:
A semaphore limits the maximum number of concurrent clients:
```c
#include <semaphore.h>
sem_t client_semaphore;
// Initialize in main():
sem_init(&client_semaphore, 0, MAX_CLIENTS);  // MAX_CLIENTS = 100
// Before creating thread:
sem_wait(&client_semaphore);   // Decrement — blocks if 0
// After thread finishes:
sem_post(&client_semaphore);   // Increment
```

### Thread Function Signature:
```c
void *client_handler(void *arg) {
    ClientSession *session = (ClientSession *)arg;
    // ... handle client commands in a loop ...
    // ... when client disconnects:
    sem_post(&client_semaphore);
    close(session->socket_fd);
    free(session);
    pthread_exit(NULL);
}
```

---

## DATA CONSISTENCY (Mandatory Concept 4)

### This is demonstrated by the INCR command.

### The Problem (Race Condition Without Mutex):
Thread A reads counter = 5
Thread B reads counter = 5  (before A writes!)
Thread A writes counter = 6
Thread B writes counter = 6  (lost update! Should be 7)

### The Solution (Atomic INCR with Mutex):
```c
void cmd_incr(ClientSession *session, char *key, char *response) {
    pthread_mutex_lock(&db_mutex);         // START critical section

    char *val = ht_get(db, key);
    int num = (val != NULL) ? atoi(val) : 0;
    num++;

    char new_val[32];
    sprintf(new_val, "%d", num);
    ht_set(db, key, new_val);

    pthread_mutex_unlock(&db_mutex);       // END critical section

    sprintf(response, ":%d\r\n", num);
}
```

The mutex ensures the read-modify-write sequence is atomic.

### Dirty Read Prevention:
Because all GET operations also lock db_mutex, a reader never sees a
half-written value from a concurrent SET.

---

## SOCKET PROGRAMMING (Mandatory Concept 5)

### Architecture:
```
[Client Terminal 1] ─────┐
[Client Terminal 2] ─────┼──► [TCP Server on port 6379] ──► [Hash Table in RAM]
[Client Terminal 3] ─────┘                │
                                           └──► [dump.rdb on disk]
```

### Server Initialization (in server.c main()):
```c
#define PORT 6379

int server_fd = socket(AF_INET, SOCK_STREAM, 0);

// Allow port reuse (prevents "Address already in use" error on restart)
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

struct sockaddr_in address;
address.sin_family      = AF_INET;
address.sin_addr.s_addr = INADDR_ANY;
address.sin_port        = htons(PORT);

bind(server_fd, (struct sockaddr *)&address, sizeof(address));
listen(server_fd, 10);

printf("Mini Redis server running on port %d\n", PORT);

while (!shutdown_flag) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) continue;

    ClientSession *session = malloc(sizeof(ClientSession));
    session->socket_fd     = client_fd;
    session->authenticated = 0;
    session->role          = READER;  // Default until AUTH

    sem_wait(&client_semaphore);

    pthread_t tid;
    pthread_create(&tid, NULL, client_handler, session);
    pthread_detach(tid);  // No need to join
}
```

### Client Connection (for testing with telnet):
```bash
telnet localhost 6379
```
Then type commands directly.

### Alternatively, a simple C client:
```c
int sock = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in serv_addr;
serv_addr.sin_family = AF_INET;
serv_addr.sin_port   = htons(6379);
inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
```

### Command Reading from Client:
```c
char buffer[1024];
int bytes = recv(session->socket_fd, buffer, sizeof(buffer)-1, 0);
buffer[bytes] = '\0';
// Remove trailing \r\n
buffer[strcspn(buffer, "\r\n")] = '\0';
// Parse and dispatch command
```

### Response Format:
```
+OK\r\n              ← Success string
-ERR message\r\n     ← Error
$5\r\nAlice\r\n      ← Bulk string (length then value)
:42\r\n              ← Integer
$-1\r\n              ← Null (key not found)
```

---

## INTER-PROCESS COMMUNICATION — IPC (Mandatory Concept 6)

### IPC Mechanism Used: Pipes (fork + pipe)

### Where IPC Is Used: BGSAVE command

### Why BGSAVE Is Different From SAVE:
- SAVE: Synchronous. Server is busy saving. No clients can be served during save.
- BGSAVE: Asynchronous. Server forks a CHILD PROCESS to do the saving.
  Parent continues serving clients normally. Child notifies parent when done via pipe.

### BGSAVE Implementation:

```c
void cmd_bgsave(ClientSession *session, char *response) {
    if (!has_permission(session, ADMIN)) {
        sprintf(response, "-ERR Permission denied\r\n");
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        sprintf(response, "-ERR pipe failed\r\n");
        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        sprintf(response, "-ERR fork failed\r\n");
        return;
    }

    if (pid == 0) {
        // ===== CHILD PROCESS =====
        close(pipefd[0]);   // Close read end (child only writes)

        save_snapshot(db);  // Do the actual disk save

        write(pipefd[1], "DONE", 4);  // Notify parent
        close(pipefd[1]);
        exit(0);            // Child exits — does NOT return to server loop

    } else {
        // ===== PARENT PROCESS =====
        close(pipefd[1]);   // Close write end (parent only reads)

        // Non-blocking: parent does NOT wait here.
        // A background watcher thread reads from pipefd[0] asynchronously.
        // (See bgsave_watcher_thread in expiry.c)

        sprintf(response, "+Background saving started\r\n");
    }
}
```

### Background Watcher Thread (reads pipe when child finishes):
```c
void *bgsave_watcher(void *arg) {
    int read_fd = *(int *)arg;
    char msg[10];
    read(read_fd, msg, 4);  // Blocks until child writes "DONE"
    msg[4] = '\0';
    printf("[%s] Background save completed: %s\n", timestamp(), msg);
    close(read_fd);
    free(arg);
    return NULL;
}
```

---

## COMPLETE COMMAND LIST (10 Commands)

### Tier 1 — Core CRUD (Must Have):

| Command            | Syntax              | Role Required | Description                              |
|--------------------|---------------------|---------------|------------------------------------------|
| AUTH               | AUTH user pass      | None          | Authenticate and set role for session    |
| SET                | SET key value       | WRITER+       | Store a key-value pair                   |
| GET                | GET key             | READER+       | Retrieve value. Returns $-1 if not found |
| DEL                | DEL key             | ADMIN only    | Delete a key                             |
| EXISTS             | EXISTS key          | READER+       | Returns :1 if exists, :0 if not          |

### Tier 2 — Differentiation (Better Than Classmates):

| Command            | Syntax              | Role Required | Description                              |
|--------------------|---------------------|---------------|------------------------------------------|
| INCR               | INCR key            | WRITER+       | Atomically increment integer value by 1  |
| APPEND             | APPEND key value    | WRITER+       | Append string to existing value          |
| EXPIRE             | EXPIRE key seconds  | WRITER+       | Set key to auto-delete after N seconds   |
| TTL                | TTL key             | READER+       | Returns remaining seconds. -1 if no TTL  |

### Tier 3 — Advanced OS Concepts (Standout):

| Command            | Syntax              | Role Required | Description                              |
|--------------------|---------------------|---------------|------------------------------------------|
| SAVE               | SAVE                | ADMIN only    | Synchronous snapshot to dump.rdb         |
| BGSAVE             | BGSAVE              | ADMIN only    | Async snapshot via fork+pipe (IPC)       |

---

## EXPIRY SYSTEM (Background Thread for EXPIRE/TTL)

### How It Works:
1. When client sends `EXPIRE counter 60`, server calls `ht_set_expiry(db, "counter", time(NULL) + 60)`.
2. A background thread (`expiry_thread`) runs in a loop, sleeping 1 second between checks.
3. Every iteration, it calls `ht_purge_expired(db)` which scans all buckets and deletes
   any entry where `entry->expiry_time != 0 && entry->expiry_time <= time(NULL)`.

### Expiry Thread:
```c
void *expiry_thread_func(void *arg) {
    while (!shutdown_flag) {
        sleep(1);
        pthread_mutex_lock(&db_mutex);
        ht_purge_expired(db);
        pthread_mutex_unlock(&db_mutex);
    }
    return NULL;
}
```

### TTL Command Implementation:
```c
void cmd_ttl(ClientSession *session, char *key, char *response) {
    pthread_mutex_lock(&db_mutex);
    time_t expiry = ht_get_expiry(db, key);
    pthread_mutex_unlock(&db_mutex);

    if (expiry == 0) {
        sprintf(response, ":-1\r\n");   // No expiry set
    } else {
        int remaining = (int)(expiry - time(NULL));
        if (remaining < 0) remaining = 0;
        sprintf(response, ":%d\r\n", remaining);
    }
}
```

---

## COMMAND PARSER

### All commands arrive as plain text strings from the client.
### Parser splits input string into tokens:

```c
typedef struct {
    char *args[10];   // Max 10 tokens per command
    int   argc;
} ParsedCommand;

void parse_command(char *input, ParsedCommand *cmd) {
    cmd->argc = 0;
    char *token = strtok(input, " \t");
    while (token != NULL && cmd->argc < 10) {
        cmd->args[cmd->argc++] = token;
        token = strtok(NULL, " \t");
    }
}
```

### Command Dispatcher (in client_handler thread):
```c
// Convert command to uppercase first for case-insensitive matching
to_uppercase(cmd.args[0]);

if      (strcmp(cmd.args[0], "AUTH")   == 0) cmd_auth(session, cmd, response);
else if (strcmp(cmd.args[0], "SET")    == 0) cmd_set(session, cmd, response);
else if (strcmp(cmd.args[0], "GET")    == 0) cmd_get(session, cmd, response);
else if (strcmp(cmd.args[0], "DEL")    == 0) cmd_del(session, cmd, response);
else if (strcmp(cmd.args[0], "EXISTS") == 0) cmd_exists(session, cmd, response);
else if (strcmp(cmd.args[0], "INCR")   == 0) cmd_incr(session, cmd, response);
else if (strcmp(cmd.args[0], "APPEND") == 0) cmd_append(session, cmd, response);
else if (strcmp(cmd.args[0], "EXPIRE") == 0) cmd_expire(session, cmd, response);
else if (strcmp(cmd.args[0], "TTL")    == 0) cmd_ttl(session, cmd, response);
else if (strcmp(cmd.args[0], "SAVE")   == 0) cmd_save(session, response);
else if (strcmp(cmd.args[0], "BGSAVE") == 0) cmd_bgsave(session, response);
else    sprintf(response, "-ERR Unknown command '%s'\r\n", cmd.args[0]);

send(session->socket_fd, response, strlen(response), 0);
```

---

## SERVER STARTUP SEQUENCE

```
1. Initialize db = ht_create()
2. Try to load dump.rdb → if exists, restore all key-value pairs into db
3. Initialize db_mutex, shutdown_mutex
4. Initialize client_semaphore (MAX_CLIENTS = 100)
5. Start expiry_thread (background key expiration)
6. Create TCP server socket, bind to port 6379, listen
7. Print: "Mini Redis server started on port 6379"
8. Enter accept() loop — wait for clients
9. For each client: malloc ClientSession, pthread_create(client_handler)
10. On SIGINT (Ctrl+C): set shutdown_flag=1, call SAVE, cleanup, exit
```

---

## SERVER SHUTDOWN (SIGINT Handler)

```c
void sigint_handler(int sig) {
    printf("\nShutting down server...\n");
    shutdown_flag = 1;
    save_snapshot(db);      // Save before exit
    printf("Data saved to dump.rdb\n");
    ht_destroy(db);
    exit(0);
}

// Register in main():
signal(SIGINT, sigint_handler);
``` 

---

## CRASH RECOVERY

When server restarts after a crash:
1. `load_snapshot(db)` is called during startup.
2. It reads dump.rdb line by line, calling `ht_set()` for each entry.
3. Entries with expired `expiry_time` are skipped on load.
4. Server continues as if it never crashed, with all data intact.

---

## MAKEFILE

```makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -g
SRCS    = src/server.c src/commands.c src/hashtable.c src/auth.c \
          src/persistence.c src/expiry.c
TARGET  = mini-redis

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET) dump.rdb server.log
```

Compile: `make`
Run: `make run` or `./mini-redis`

---

## TESTING PROCEDURE

### Terminal 1 — Start Server:
```bash
./mini-redis
```

### Terminal 2 — Admin Client:
```bash
telnet localhost 6379
AUTH admin admin123
SET counter 0
INCR counter
INCR counter
GET counter          # Should return 2
EXPIRE counter 15
TTL counter          # Should return ~15
SAVE
BGSAVE
```

### Terminal 3 — Writer Client (run SIMULTANEOUSLY with Terminal 2):
```bash
telnet localhost 6379
AUTH writer writer123
SET name Alice
APPEND name " Smith"
GET name             # Should return "Alice Smith"
INCR counter         # Concurrent INCR — should not cause lost update
```

### Terminal 4 — Reader Client:
```bash
telnet localhost 6379
AUTH reader reader123
GET name             # Should return "Alice Smith"
EXISTS counter       # Should return :1
SET foo bar          # Should return -ERR Permission denied
DEL name             # Should return -ERR Permission denied
TTL counter          # Should return remaining seconds
```

### Testing Crash Recovery:
```bash
# Start server
./mini-redis
# Connect as admin and set data
# Press Ctrl+C to kill server
# Restart server
./mini-redis
# GET the keys — they should still be there
```

### Testing BGSAVE Non-Blocking:
```bash
# In admin terminal: send BGSAVE
BGSAVE
# Immediately in writer terminal: send SET — should respond instantly
# Proving that BGSAVE's child process does not block parent
```

---

## HOW ALL 6 MANDATORY OS CONCEPTS ARE COVERED

| # | Mandatory Concept         | How It Is Covered in This Project                                                   |
|---|---------------------------|-------------------------------------------------------------------------------------|
| 1 | Role-Based Authorization  | AUTH command sets ADMIN/WRITER/READER role per session. Every command checks role   |
|   |                           | before executing. DEL and SAVE restricted to ADMIN. GET allowed to all.             |
| 2 | File Locking              | SAVE and BGSAVE use flock(LOCK_EX) before writing dump.rdb. Startup uses            |
|   |                           | flock(LOCK_SH) when reading. Prevents file corruption from concurrent saves.         |
| 3 | Concurrency Control       | One pthread per client. Global db_mutex (mutex) protects hash table. Semaphore      |
|   |                           | limits max concurrent clients. Expiry thread also runs concurrently.                |
| 4 | Data Consistency          | INCR command demonstrates atomic read-modify-write under mutex. Prevents race        |
|   |                           | conditions, dirty reads, and lost updates under concurrent client access.            |
| 5 | Socket Programming        | TCP server on port 6379 using socket/bind/listen/accept/send/recv. Each client      |
|   |                           | connects independently. Classic multi-client client-server model.                   |
| 6 | IPC (Inter-Process Comm)  | BGSAVE forks a child process. Parent continues serving clients. Child saves data     |
|   |                           | to disk. Child notifies parent via pipe when done. Parent-child: separate processes. |

---

## IMPORTANT IMPLEMENTATION RULES

1. NEVER use external libraries for the hash table. It is built from scratch using
   arrays and linked lists.

2. ALWAYS lock db_mutex before accessing db and unlock immediately after. Never hold
   the lock longer than necessary.

3. The child process in BGSAVE must call exit() when done. It must NOT return to the
   server's accept() loop.

4. Load dump.rdb at server startup BEFORE the accept() loop begins.

5. The expiry thread must lock db_mutex before calling ht_purge_expired().

6. AUTH must be the first command a client sends. All other commands should check
   session->authenticated == 1 and return -ERR Not authenticated if not.

7. All responses end with \r\n as per standard protocol.

8. Use pthread_detach() after pthread_create() for client threads so memory is
   automatically freed when thread exits.

9. Log every command with a timestamp to server.log for debugging and demonstration.

---

## WHAT NOT TO IMPLEMENT (Scope Control)

- Do NOT implement Redis Cluster or replication
- Do NOT implement Lua scripting
- Do NOT implement pub/sub messaging
- Do NOT implement sorted sets, lists, or hash data types (only string key-value)
- Do NOT implement a binary protocol — plain text is fine for this project
- Do NOT implement SSL/TLS encryption
- Keep it simple: one server, multiple clients, one dump file

---

## CURRENT IMPLEMENTATION STATUS

(Update this section as components are completed)

- [ ] hashtable.c / hashtable.h — Custom hash table from scratch
- [ ] auth.c / auth.h — Role system and AUTH command
- [ ] server.c — TCP server, thread spawning, semaphore, signal handler
- [ ] commands.c — SET, GET, DEL, EXISTS, INCR, APPEND, EXPIRE, TTL
- [ ] persistence.c — SAVE, BGSAVE, load_snapshot with file locking
- [ ] expiry.c — Background TTL checker thread
- [ ] Makefile
- [ ] Testing with 3 simultaneous clients
- [ ] Crash recovery test

---

END OF CONTEXT DOCUMENT
