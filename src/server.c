/*
 * =============================================================================
 * server.c — Mini Redis Server
 * =============================================================================
 *
 * EGC 301P Operating Systems Lab Mini Project
 *
 * This file is the heart of the Mini Redis server. It implements:
 *   - OS Concept 1: Role-Based Authorization  (via auth.c + users.conf)
 *   - OS Concept 2: File Locking              (via persistence.c + flock)
 *   - OS Concept 3: Concurrency Control       (pthreads + semaphore)
 *   - OS Concept 4: Data Consistency          (db_mutex on every db access)
 *   - OS Concept 5: Socket Programming        (TCP server on port 6379)
 *   - OS Concept 6: IPC                       (fork + pipe in BGSAVE)
 *
 * Architecture (mirrors real Redis):
 *   Main thread  →  accept() loop  →  pthread_create() per client
 *   Each client thread  →  auth gate  →  command dispatcher  →  db (mutex-protected)
 *   Background expiry thread  →  purges expired keys every second
 *   BGSAVE  →  fork() child saves to disk  →  notifies parent via pipe
 *
 * Compile:
 *   gcc -Wall -Wextra -pthread -g -o mini-redis \
 *       server.c commands.c hashtable.c auth.c persistence.c expiry.c
 *
 * Run:
 *   ./mini-redis
 *
 * Connect (test):
 *   telnet localhost 6379
 * =============================================================================
 */

/* ─── Standard headers ───────────────────────────────────────────────────── */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

/* ─── Networking headers ─────────────────────────────────────────────────── */
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ─── Threading + sync headers ──────────────────────────────────────────── */
#include <pthread.h>
#include <semaphore.h>

/* ─── Project headers ────────────────────────────────────────────────────── */
#include "hashtable.h"
#include "auth.h"
#include "commands.h"
#include "persistence.h"
#include "expiry.h"

/* =============================================================================
 * CONFIGURATION CONSTANTS
 * Change these to adjust server behaviour.
 * ============================================================================= */
#define SERVER_PORT       6379                    /* Default Redis port                  */
#define MAX_CLIENTS       100                     /* Semaphore limit - max live clients  */
#define BACKLOG           511                     /* TCP listen() backlog queue size     */
#define RECV_BUFFER_SIZE  2048                    /* Max bytes read per command          */
#define SEND_BUFFER_SIZE  4096                    /* Max bytes sent per response         */
#define USERS_CONF        "users.conf"            /* ACL credential file                 */
#define DUMP_FILE         "dump.rdb"              /* Persistence snapshot file           */
#define LOG_FILE          "server.log"            /* Server activity log                 */
#define CLIENT_SEM_NAME   "/mini-redis-clients"   /* Named semaphore — macOS compatible  */

/* =============================================================================
 * GLOBAL SHARED STATE
 *
 * These variables are shared across ALL threads.
 * Every access to `db` MUST be protected by `db_mutex`.
 * This is the core of OS Concept 3 (Concurrency) and Concept 4 (Consistency).
 * ============================================================================= */

/* The in-memory key-value store — custom hash table, built from scratch */
HashTable *db;

/*
 * db_mutex — the single most important synchronization primitive.
 *
 * WHY: Without this, two threads doing INCR simultaneously would both read
 * value "5", both compute "6", and both write "6". The counter increments
 * only once instead of twice. That is a lost update — a data consistency bug.
 *
 * HOW: Every command handler calls pthread_mutex_lock(&db_mutex) before
 * touching `db` and pthread_mutex_unlock(&db_mutex) immediately after.
 * Only one thread can hold this lock at a time. Others block until it releases.
 *
 * OS Concept 3: Concurrency Control
 * OS Concept 4: Data Consistency
 */
pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * shutdown_flag — set to 1 by the SIGINT handler (Ctrl+C).
 * The accept() loop and the expiry thread both check this flag to exit cleanly.
 * Protected by shutdown_mutex to avoid a race condition on the flag itself.
 */
volatile sig_atomic_t shutdown_flag = 0;
pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * client_semaphore — limits the number of simultaneously connected clients.
 *
 * WHY: Without a cap, a flood of connections would spawn thousands of threads,
 * exhausting system memory and crashing the server (a resource exhaustion attack).
 *
 * HOW: Initialized to MAX_CLIENTS (100). sem_wait() decrements it before creating
 * a thread — if it reaches 0 the main thread blocks, refusing new connections
 * until an existing client disconnects and calls sem_post().
 *
 * OS Concept 3: Concurrency Control (via semaphore)
 */
sem_t *client_semaphore;   /* Named semaphore — sem_open() on macOS */

/* Log file handle — opened once at startup, closed at shutdown */
static FILE *log_fp = NULL;

/* Server socket fd — global so sigint_handler can close it */
static int server_fd = -1;

/* =============================================================================
 * LOGGING
 *
 * log_event() is thread-safe because fprintf to a FILE* is internally buffered
 * and we flush after every write. For production you would use a mutex here too,
 * but for this project the OS guarantees atomic writes for small writes on Linux.
 * ============================================================================= */
void log_event(const char *level, const char *username, const char *message) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    /* Write to log file */
    if (log_fp) {
        fprintf(log_fp, "[%s] [%s] [%s] %s\n", timestamp, level, username, message);
        fflush(log_fp);
    }

    /* Also print to stdout so you can see activity while running */
    printf("[%s] [%s] [%s] %s\n", timestamp, level, username, message);
}

/* =============================================================================
 * SIGNAL HANDLER — SIGINT (Ctrl+C)
 *
 * Real Redis calls this a "graceful shutdown". When you press Ctrl+C:
 *   1. Set shutdown_flag so all loops exit.
 *   2. Save in-memory data to dump.rdb so nothing is lost.
 *   3. Close the server socket.
 *   4. Free all memory.
 *   5. Exit cleanly.
 *
 * signal() is async-signal-safe. We only set a volatile flag here and do the
 * real cleanup in main() after the accept loop exits. The save_snapshot() call
 * here is intentional — we need to save before any other thread touches the db.
 * ============================================================================= */
void sigint_handler(int sig) {
    (void)sig;  /* Suppress unused parameter warning */

    /*
     * Write 1 directly, safe because sig_atomic_t writes are atomic on all
     * POSIX platforms. This signals the accept() loop and expiry thread to stop.
     */
    shutdown_flag = 1;

    /*
     * Close the server socket. This unblocks accept() which is otherwise
     * sleeping inside the kernel waiting for connections.
     * accept() will return -1 with errno = EBADF, which we check and break on.
     */
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    printf("\n[SERVER] SIGINT received. Initiating graceful shutdown...\n");
}

/* =============================================================================
 * CLIENT HANDLER THREAD
 *
 * This function runs in its own pthread for every connected client.
 * It is the client's entire lifetime on the server:
 *   connect → receive commands → dispatch → send responses → disconnect
 *
 * OS Concept 3: This function IS the concurrency — multiple instances run
 *               simultaneously, one per client.
 * OS Concept 1: Every command checks has_permission() via the dispatcher.
 * OS Concept 4: Every db access inside handlers is mutex-protected.
 *
 * arg: pointer to a heap-allocated ClientSession (we own it, we free it)
 * ============================================================================= */
void *client_handler(void *arg) {
    ClientSession *session = (ClientSession *)arg;

    char recv_buf[RECV_BUFFER_SIZE];
    char send_buf[SEND_BUFFER_SIZE];
    char log_msg[512];

    /* Log the new connection */
    char peer_addr[INET_ADDRSTRLEN];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    getpeername(session->socket_fd, (struct sockaddr *)&peer, &peer_len);
    inet_ntop(AF_INET, &peer.sin_addr, peer_addr, sizeof(peer_addr));

    snprintf(log_msg, sizeof(log_msg),
             "New connection from %s:%d (fd=%d)",
             peer_addr, ntohs(peer.sin_port), session->socket_fd);
    log_event("CONNECT", "unknown", log_msg);

    /*
     * Greet the client with a persistent interactive menu.
     * This makes it easy to demonstrate all commands over raw nc/telnet.
     */
    const char *banner =
        "\r\n--- Available Commands ---\r\n"
        "AUTH <user> <pass> | SET <k> <v> | GET <k> | DEL <k> | EXISTS <k>\r\n"
        "INCR <k> | APPEND <k> <v> | EXPIRE <k> <sec> | TTL <k> | SAVE | QUIT\r\n"
        "Mini-Redis> ";
    send(session->socket_fd, banner, strlen(banner), 0);

    /*
     * ── MAIN COMMAND LOOP ──────────────────────────────────────────────────
     * Stay in this loop until:
     *   (a) The client disconnects (recv returns 0)
     *   (b) A network error occurs (recv returns -1)
     *   (c) The client sends QUIT
     *   (d) The server is shutting down (shutdown_flag == 1)
     */
    while (!shutdown_flag) {
        memset(recv_buf, 0, sizeof(recv_buf));
        memset(send_buf, 0, sizeof(send_buf));

        /*
         * recv() blocks this thread until the client sends data.
         * Returns: number of bytes received
         *          0 if client disconnected cleanly
         *         -1 on error
         *
         * This is OS Concept 5: Socket Programming.
         * The kernel delivers bytes sent by the client over TCP into recv_buf.
         */
        int bytes_received = recv(session->socket_fd,
                                  recv_buf,
                                  sizeof(recv_buf) - 1,
                                  0);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                log_event("DISCONNECT", session->username, "Client disconnected cleanly");
            } else {
                snprintf(log_msg, sizeof(log_msg),
                         "recv() error: %s", strerror(errno));
                log_event("ERROR", session->username, log_msg);
            }
            break;
        }

        /* Null-terminate the received data */
        recv_buf[bytes_received] = '\0';

        /*
         * Strip trailing \r\n (telnet sends \r\n, some clients send just \n).
         * strcspn finds the first occurrence of \r or \n and replaces it with \0.
         */
        recv_buf[strcspn(recv_buf, "\r\n")] = '\0';

        /* Skip empty lines (user just pressed Enter) */
        if (strlen(recv_buf) == 0) continue;

        /* Log the raw command received */
        snprintf(log_msg, sizeof(log_msg), "CMD: %s", recv_buf);
        log_event("CMD", session->username[0] ? session->username : "unauthenticated",
                  log_msg);

        /*
         * ── COMMAND DISPATCH ────────────────────────────────────────────────
         * dispatch_command() lives in commands.c.
         * It parses recv_buf, checks permissions via has_permission(),
         * executes the correct handler, and writes the response into send_buf.
         
         * The dispatcher returns 0 normally, or -1 if the client sent QUIT.
         */
        int quit = dispatch_command(session, recv_buf, send_buf, sizeof(send_buf));

        /*
         * Send the response back to the client over the TCP socket.
         * OS Concept 5: Socket Programming.
         */
        if (strlen(send_buf) > 0) {
            send(session->socket_fd, send_buf, strlen(send_buf), 0);
        }

        /* Client sent QUIT — exit loop cleanly */
        if (quit == -1) {
            log_event("QUIT", session->username, "Client issued QUIT");
            break;
        }

        /* Re-send the prompt after every response so the user always
         * knows the server is waiting for the next command. */
        const char *prompt = "Mini-Redis> ";
        send(session->socket_fd, prompt, strlen(prompt), 0);
    }

    /* ── CLEANUP ─────────────────────────────────────────────────────────── */

    close(session->socket_fd);

    /*
     * sem_post() increments the semaphore — signals that one slot is free.
     * If the main thread is blocked in sem_wait() because MAX_CLIENTS was
     * reached, this wakes it up to accept the next connection.
     *
     * OS Concept 3: Concurrency Control (semaphore release)
     */
    sem_post(client_semaphore);

    snprintf(log_msg, sizeof(log_msg),
             "Thread for fd=%d cleaned up", session->socket_fd);
    log_event("THREAD", session->username, log_msg);

    /* Free the session — we allocated it in main, we free it here */
    free(session);

    /*
     * pthread_exit(NULL) terminates this thread cleanly.
     * Because we called pthread_detach() in main(), the kernel automatically
     * reclaims this thread's stack and resources. No pthread_join() needed.
     */
    pthread_exit(NULL);
}

/* =============================================================================
 * SERVER INITIALIZATION
 *
 * Creates, configures, and binds the TCP server socket.
 * Returns the server file descriptor on success, -1 on failure.
 *
 * This is OS Concept 5: Socket Programming — the server-side setup.
 * ============================================================================= */
static int init_server_socket(void) {
    int fd;

    /*
     * socket(AF_INET, SOCK_STREAM, 0)
     *   AF_INET    = IPv4
     *   SOCK_STREAM = TCP (reliable, ordered, stream-based)
     *   0          = default protocol for SOCK_STREAM (which is TCP)
     *
     * Returns a file descriptor — just like open() for files.
     */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[SERVER] socket() failed");
        return -1;
    }

    /*
     * SO_REUSEADDR — critical for development.
     *
     * WHY: Without this, if you kill the server and restart immediately,
     * bind() fails with "Address already in use". This happens because the
     * kernel keeps the port in TIME_WAIT state for ~60 seconds after close().
     * SO_REUSEADDR tells the kernel to allow immediate reuse of the port.
     */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[SERVER] setsockopt(SO_REUSEADDR) failed");
        close(fd);
        return -1;
    }

    /*
     * bind() — assigns the socket to an address (IP + port).
     *   sin_family      = AF_INET (IPv4)
     *   sin_addr.s_addr = INADDR_ANY (accept connections on any network interface)
     *   sin_port        = htons(PORT) — host-to-network byte order conversion
     *
     * htons() is necessary because network byte order is big-endian but x86
     * CPUs are little-endian. htons("host to network short") converts correctly.
     */
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(SERVER_PORT);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("[SERVER] bind() failed — is port 6379 already in use?");
        close(fd);
        return -1;
    }

    /*
     * listen() — marks the socket as passive (server-side).
     * BACKLOG = 10: up to 10 connection requests can queue up while the main
     * thread is busy processing a previous accept() call. Additional connections
     * beyond the backlog are refused by the kernel.
     */
    if (listen(fd, BACKLOG) < 0) {
        perror("[SERVER] listen() failed");
        close(fd);
        return -1;
    }

    return fd;
}

/* =============================================================================
 * MAIN — SERVER ENTRY POINT
 *
 * Startup sequence (mirrors real Redis startup):
 *   1. Open log file
 *   2. Register SIGINT handler
 *   3. Load users from users.conf (ACL system)
 *   4. Initialize shared data structures (db, mutex, semaphore)
 *   5. Load dump.rdb if it exists (crash recovery)
 *   6. Start background expiry thread
 *   7. Create TCP server socket
 *   8. Enter accept() loop — main server loop
 *   9. On shutdown: save data, clean up, exit
 * ============================================================================= */

 void init_logging(void)
 {
    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp) {
        fprintf(stderr, "[SERVER] WARNING: Cannot open %s for logging. "
                        "Continuing without file log.\n", LOG_FILE);
        /* Non-fatal — we can still run, just without file logging */
    }

    log_event("START", "server", "═══ Mini Redis Server starting ═══");
 }

 void register_signals(void)
 {
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);  /* Also handle kill command */
    signal(SIGPIPE, SIG_IGN);         /* Ignore broken pipe — client disconnected mid-send. */

    log_event("INIT", "server", "Signal handlers registered");
 }

void setup_server_state(void)
{
        /*
     * OS Concept 1: Role-Based Authorization
     *
     * load_users_from_file() reads users.conf, parses each line, and populates
     * the global user_table[] in auth.c. If the file is missing or empty,
     * the server refuses to start — there's no fallback to hardcoded credentials.
     *
     * This is identical in philosophy to how Redis 6.0 ACL files work.
     */
    if (!load_users_from_file(USERS_CONF)) {
        fprintf(stderr,
            "\n[SERVER] FATAL: Could not load users from '%s'.\n"
            "         Create a users.conf file in this directory with format:\n"
            "           username password role\n"
            "         Roles: 0=ADMIN  1=WRITER  2=READER\n"
            "           admin  admin123  0\n"
            "           alice  pass123   1\n"
            "           bob    readpass  2\n\n",
            USERS_CONF);
        exit(EXIT_FAILURE);
    }

    log_event("INIT", "server", "ACL users loaded from users.conf");

    /*
     * ht_create() allocates and zero-initializes the hash table.
     * From this point on, EVERY access to `db` must hold db_mutex.
     *
     * OS Concept 3 + 4: Concurrency + Data Consistency
     */
    db = ht_create();
    if (!db) {
        fprintf(stderr, "[SERVER] FATAL: Failed to allocate hash table\n");
        exit(EXIT_FAILURE);
    }
    log_event("INIT", "server", "In-memory hash table created (256 buckets)");

    /*
     * Initialize the semaphore.
     *
     * macOS does not implement unnamed POSIX semaphores (sem_init always
     * returns ENOSYS). We use a named semaphore instead, which is fully
     * supported on both macOS and Linux.
     *
     * sem_open() parameters:
     *   CLIENT_SEM_NAME  — unique name in the kernel's semaphore namespace
     *   O_CREAT          — create if it doesn't exist
     *   0644             — permissions (owner rw, others r)
     *   MAX_CLIENTS      — initial value (number of concurrent client slots)
     *
     * sem_unlink() is called first to remove any stale semaphore left over
     * from a previous crash (prevents sem_open from inheriting old state).
     *
     * OS Concept 3: Concurrency Control (semaphore)
     */
    sem_unlink(CLIENT_SEM_NAME);  /* Remove stale semaphore from a prior crash */
    client_semaphore = sem_open(CLIENT_SEM_NAME, O_CREAT, 0644, MAX_CLIENTS);
    if (client_semaphore == SEM_FAILED) {
        perror("[SERVER] sem_open() failed");
        ht_destroy(db);
        exit(EXIT_FAILURE);
    }
    log_event("INIT", "server", "Client semaphore initialized (max=100)");


        int keys_loaded = load_snapshot(db, DUMP_FILE);
    if (keys_loaded > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Restored %d key(s) from %s", keys_loaded, DUMP_FILE);
        log_event("RESTORE", "server", msg);
    } else {
        log_event("RESTORE", "server",
                  "No existing dump.rdb found — starting with empty database");
    }

    /* ── STEP 6: Start background expiry thread ──────────────────────────── */
    /*
     * The expiry thread runs expiry_thread_func() in expiry.c.
     * It sleeps for 1 second, then locks db_mutex, calls ht_purge_expired(),
     * unlocks, and repeats. This is how TTL/EXPIRE commands are enforced.
     *
     * pthread_detach() means we don't need to pthread_join() it later —
     * the kernel cleans it up when the process exits.
     *
     * OS Concept 3: Background concurrent thread
     */
    pthread_t expiry_tid;
    if (pthread_create(&expiry_tid, NULL, expiry_thread_func, NULL) != 0) {
        perror("[SERVER] pthread_create() for expiry thread failed");
        /* Non-fatal — server still works, just EXPIRE won't auto-delete */
    } else {
        pthread_detach(expiry_tid);
        log_event("INIT", "server", "Background expiry thread started");
    }

    /* ── STEP 7: Create TCP server socket ────────────────────────────────── */
    /*
     * OS Concept 5: Socket Programming
     *
     * init_server_socket() creates, configures (SO_REUSEADDR), binds to
     * port 6379, and calls listen(). It returns the server file descriptor.
     */
    server_fd = init_server_socket();
    if (server_fd < 0) {
        /* Error already printed inside init_server_socket() */
        sem_close(client_semaphore);
        sem_unlink(CLIENT_SEM_NAME);
        ht_destroy(db);
        exit(EXIT_FAILURE);
    }

    char startup_msg[128];
    snprintf(startup_msg, sizeof(startup_msg),
             "Listening on port %d (fd=%d)", SERVER_PORT, server_fd);
    log_event("READY", "server", startup_msg);

    printf("\n");
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║       Mini Redis Server v1.0         ║\n");
    printf("  ║   Listening on port %-5d             ║\n", SERVER_PORT);
    printf("  ║   Connect: telnet localhost %d       ║\n", SERVER_PORT);
    printf("  ║   Stop:    Ctrl+C                    ║\n");
    printf("  ╚══════════════════════════════════════╝\n\n");

}

void run_accept_loop(void)
{
    /* ── STEP 8: Main accept() loop ──────────────────────────────────────── */
    /*
     * This is the core of OS Concept 5: Socket Programming.
     *
     * The main thread does NOTHING except:
     *   (a) Block in accept() until a client connects
     *   (b) Acquire the semaphore (or block if MAX_CLIENTS reached)
     *   (c) Allocate a ClientSession for the new client
     *   (d) Spawn a pthread to handle the client
     *   (e) Immediately loop back to accept()
     *
     * The main thread NEVER processes commands. Commands run inside client threads.
     * This is identical to how PostgreSQL handles connections.
     *
     * Real Redis uses an event loop (epoll) instead of threads for scalability,
     * but the thread-per-client model here is cleaner to understand and debug,
     * and perfectly demonstrates OS concurrency concepts.
     */
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (!shutdown_flag) {
        /*
         * accept() blocks here until a client connects.
         * When a client connects, the kernel:
         *   1. Completes the TCP three-way handshake (SYN, SYN-ACK, ACK)
         *   2. Creates a new socket (client_fd) for this specific connection
         *   3. Returns client_fd to us
         *
         * server_fd remains open and accepts future connections.
         * client_fd is unique to this one client.
         *
         * OS Concept 5: Socket Programming
         */
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);

        /* Check if accept() failed because we shut down */
        if (client_fd < 0) {
            if (shutdown_flag) {
                break;  /* Expected — sigint_handler closed server_fd */
            }
            if (errno == EINTR) {
                continue;  /* Interrupted by signal — retry */
            }
            perror("[SERVER] accept() failed");
            continue;
        }

        /*
         * sem_wait() — try to decrement the semaphore.
         *
         * If the semaphore value is > 0 (fewer than MAX_CLIENTS connected):
         *   sem_wait() decrements it and returns immediately.
         *
         * If the semaphore value is 0 (MAX_CLIENTS already connected):
         *   sem_wait() BLOCKS the main thread here until a client disconnects
         *   and calls sem_post() from its client_handler thread.
         *
         * During this block, no new connections are accepted. The OS keeps
         * them queued in the BACKLOG.
         *
         * OS Concept 3: Concurrency Control (semaphore)
         */
        if (sem_wait(client_semaphore) != 0) {
            /* sem_wait was interrupted (e.g., by signal during shutdown) */
            close(client_fd);
            break;
        }

        /*
         * Allocate ClientSession on the heap.
         *
         * WHY heap, not stack? The stack variable would be destroyed as soon as
         * main() moves to the next iteration of the loop. The client thread needs
         * the session to persist for the entire duration of the client connection.
         * The client_handler thread is responsible for freeing this memory.
         */
        ClientSession *session = malloc(sizeof(ClientSession));
        if (!session) {
            fprintf(stderr, "[SERVER] malloc() failed for ClientSession\n");
            close(client_fd);
            sem_post(client_semaphore);  /* Return the slot we just took */
            continue;
        }

        /* Initialize the session — client starts unauthenticated */
        memset(session, 0, sizeof(ClientSession));
        session->socket_fd     = client_fd;
        session->authenticated = 0;
        session->role          = READER;  /* Most restrictive default */
        strncpy(session->username, "unauthenticated", sizeof(session->username) - 1);

        /*
         * pthread_create() spawns a new OS thread running client_handler().
         *
         * The new thread gets its own stack and runs client_handler(session)
         * concurrently with the main thread and all other client threads.
         *
         * &tid:         Thread ID (we don't need it after detach)
         * NULL:         Default thread attributes (default stack size, etc.)
         * client_handler: The function the thread will run
         * session:      Argument passed to client_handler — our ClientSession*
         *
         * OS Concept 3: Concurrency Control (pthread)
         */
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, session) != 0) {
            perror("[SERVER] pthread_create() for client failed");
            free(session);
            close(client_fd);
            sem_post(client_semaphore);
            continue;
        }

        /*
         * pthread_detach() — tells the kernel to automatically reclaim this
         * thread's resources when it exits, without needing pthread_join().
         *
         * WHY: If we called pthread_join(), the main thread would block waiting
         * for EACH client to disconnect — defeating the purpose of threading.
         * With detach, threads clean themselves up independently.
         */
        pthread_detach(tid);

        char conn_msg[128];
        snprintf(conn_msg, sizeof(conn_msg),
                 "Client accepted (fd=%d) from %s:%d",
                 client_fd,
                 inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port));
        log_event("ACCEPT", "server", conn_msg);
    }

}

void perform_graceful_shutdown(void)
{
    /* ── STEP 9: Graceful shutdown ───────────────────────────────────────── */
    /*
     * We reach here when shutdown_flag == 1 (set by sigint_handler).
     *
     * Real Redis calls this "graceful shutdown". We:
     *   1. Save all in-memory data to dump.rdb before exiting.
     *   2. Destroy synchronization primitives.
     *   3. Free heap memory.
     *   4. Close file handles.
     *
     * Note: We cannot wait for all client threads to finish (that would require
     * tracking all thread IDs). Active clients will get connection-reset errors.
     * This is acceptable — they can reconnect after the server restarts and
     * will find their data intact in dump.rdb.
     */
    printf("\n[SERVER] Shutting down gracefully...\n");
    log_event("SHUTDOWN", "server", "Saving data before exit...");

    /*
     * OS Concept 2: File Locking
     * 
     * save_snapshot() acquires an exclusive flock() before writing dump.rdb.
     * This prevents any concurrent BGSAVE child process from writing at the
     * same time and corrupting the file.
     */
    pthread_mutex_lock(&db_mutex);
    int saved = save_snapshot(db, DUMP_FILE);
    pthread_mutex_unlock(&db_mutex);

    if (saved >= 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Saved %d key(s) to %s", saved, DUMP_FILE);
        log_event("SHUTDOWN", "server", msg);
    } else {
        log_event("SHUTDOWN", "server", "WARNING: Failed to save snapshot");
    }

    /* Clean up synchronization primitives */
    sem_close(client_semaphore);
    sem_unlink(CLIENT_SEM_NAME);
    pthread_mutex_destroy(&db_mutex);
    pthread_mutex_destroy(&shutdown_mutex);

    /* Free the hash table and all stored key-value pairs */
    ht_destroy(db);

    log_event("SHUTDOWN", "server", "═══ Mini Redis Server stopped ═══");

    if (log_fp) {
        fclose(log_fp);
    }

    printf("[SERVER] Shutdown complete. Goodbye.\n");
    exit(EXIT_SUCCESS);
}

int main(void) 
{

    /* STEP 1: Open log file */
    init_logging();

    /*  STEP 2: Register signal handler  */
    register_signals();

    setup_server_state();
    
    run_accept_loop();
    
    perform_graceful_shutdown();
    return EXIT_SUCCESS;


}