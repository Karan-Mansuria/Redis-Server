// =============================================================================
// persistence.c — Snapshot persistence: SAVE, BGSAVE, load_snapshot
// Location: mini-redis/persistence.c
// =============================================================================
//
// OS CONCEPTS DEMONSTRATED IN THIS FILE:
//
//   Concept 2 — File Locking:
//     flock(LOCK_EX) is called before every write to dump.rdb.
//     flock(LOCK_SH) is called before every read from dump.rdb.
//     This prevents file corruption if two processes try to write simultaneously
//     (e.g., a SAVE and BGSAVE child running at the same time).
//
//   Concept 6 — Inter-Process Communication (IPC):
//     bgsave_async() calls fork() to create a child process.
//     The child saves data and writes "DONE" into a pipe.
//     The parent reads from the pipe in a detached watcher thread.
//     The parent and child are separate processes communicating via the pipe.
//
// DUMP FILE FORMAT (dump.rdb — plain text for readability):
//   # Mini-Redis snapshot — <timestamp>
//   <key> <value> <expiry_time_t>
//   ...
//
//   expiry_time = 0 means no expiry.
//   Lines starting with '#' are comments and are skipped on load.
//   Keys and values cannot contain spaces (project-scope limitation).
//
// WHY flock() INSTEAD OF POSIX LOCKS (fcntl)?
//   flock() is simpler and sufficient for this project. It locks the whole file
//   (not byte ranges). It is released automatically if the process crashes,
//   which is a useful safety property for this use case.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/file.h>   // flock()
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

#include "persistence.h"
#include "hashtable.h"
#include "globals.h"

// =============================================================================
// INTERNAL HELPER: write_entries_to_fd
//
// Iterates every bucket and every entry in the hash table, writing each
// key-value-expiry line to the file descriptor fd.
//
// Called from both save_snapshot() and the child process in bgsave_async().
//
// Returns the number of keys written.
//
// NOTE: The caller is responsible for holding db_mutex before calling this.
//       We do not lock inside here to avoid double-locking.
// =============================================================================
static int write_entries_to_fd(HashTable *ht, int fd) {
    int count = 0;
    char line[4096];

    // Write a header comment with the snapshot timestamp
    time_t now = time(NULL);
    char timestamp_str[64];
    strftime(timestamp_str, sizeof(timestamp_str),
             "%Y-%m-%d %H:%M:%S", localtime(&now));
    snprintf(line, sizeof(line), "# Mini-Redis snapshot — %s\n", timestamp_str);
    write(fd, line, strlen(line));

    // Iterate all 256 buckets
    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *entry = ht->buckets[i];

        // Walk the linked list at this bucket
        while (entry != NULL) {
            // Skip keys with no value (should never happen, but defensive)
            if (entry->key == NULL || entry->value == NULL) {
                entry = entry->next;
                continue;
            }

            // Skip already-expired keys — no point saving them
            if (entry->expiry_time != 0 && entry->expiry_time <= time(NULL)) {
                entry = entry->next;
                continue;
            }

            // Format: "key value expiry_time\n"
            // expiry_time is a Unix timestamp (long), 0 = no expiry
            snprintf(line, sizeof(line), "%s %s %ld\n",
                     entry->key,
                     entry->value,
                     (long)entry->expiry_time);

            // write() to a file descriptor (not fprintf to a FILE*)
            // because we obtained fd via open() for use with flock()
            ssize_t written = write(fd, line, strlen(line));
            if (written < 0) {
                perror("[PERSISTENCE] write() failed");
                return -1;
            }

            count++;
            entry = entry->next;
        }
    }

    return count;
}

// =============================================================================
// save_snapshot — Synchronous save with exclusive file lock
//
// Called by the SAVE command and during graceful shutdown.
// Blocks the calling thread until the write completes.
//
// OS Concept 2: File Locking
//   flock(fd, LOCK_EX) — acquires exclusive lock.
//     If another process (e.g., a BGSAVE child) holds the lock, this call
//     BLOCKS until that process releases it. This guarantees that only one
//     writer touches dump.rdb at a time, preventing file corruption.
//   flock(fd, LOCK_UN) — releases the lock explicitly.
//     flock also releases automatically when fd is closed.
//
// Returns: number of keys saved, or -1 on error.
// =============================================================================
int save_snapshot(HashTable *ht, const char *filepath) {
    // O_WRONLY | O_CREAT | O_TRUNC:
    //   O_WRONLY — open for writing only
    //   O_CREAT  — create the file if it doesn't exist
    //   O_TRUNC  — truncate (empty) the file before writing fresh data
    // 0644 = owner read+write, group+others read-only
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("[PERSISTENCE] open() failed for save_snapshot");
        return -1;
    }

    // ── ACQUIRE EXCLUSIVE FILE LOCK ───────────────────────────────────────────
    // OS Concept 2: File Locking
    //
    // LOCK_EX = exclusive lock (only one holder at a time)
    // This call BLOCKS if another process holds LOCK_EX or LOCK_SH on this file.
    // It unblocks when they release the lock.
    if (flock(fd, LOCK_EX) != 0) {
        perror("[PERSISTENCE] flock(LOCK_EX) failed");
        close(fd);
        return -1;
    }

    printf("[PERSISTENCE] Exclusive file lock acquired on %s\n", filepath);

    // Write all entries to the file
    // db_mutex must be held by the caller before calling save_snapshot()
    // when called from dispatcher.c (SAVE command) or server.c shutdown.
    int count = write_entries_to_fd(ht, fd);

    // ── RELEASE FILE LOCK ─────────────────────────────────────────────────────
    // Always release even if write_entries_to_fd returned an error
    flock(fd, LOCK_UN);
    printf("[PERSISTENCE] File lock released on %s\n", filepath);

    close(fd);

    if (count >= 0) {
        printf("[PERSISTENCE] Saved %d key(s) to %s\n", count, filepath);
    }

    return count;
}

// =============================================================================
// load_snapshot — Load key-value pairs from dump.rdb into the hash table
//
// Called once at server startup before the accept() loop begins.
// No mutex is needed here because no client threads exist yet at startup.
//
// OS Concept 2: File Locking
//   flock(fd, LOCK_SH) — shared read lock.
//   Multiple readers can hold LOCK_SH simultaneously.
//   LOCK_SH blocks if a writer holds LOCK_EX.
//   This protects against a race where a BGSAVE child writes while we read.
//
// Returns: number of keys loaded, 0 if file not found, -1 on error.
// =============================================================================
int load_snapshot(HashTable *ht, const char *filepath) {
    // O_RDONLY — open for reading only
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            // File does not exist — first run, not an error
            printf("[PERSISTENCE] No %s found — starting fresh\n", filepath);
            return 0;
        }
        perror("[PERSISTENCE] open() failed for load_snapshot");
        return -1;
    }

    // ── ACQUIRE SHARED FILE LOCK ──────────────────────────────────────────────
    // OS Concept 2: File Locking
    //
    // LOCK_SH = shared lock (multiple readers allowed simultaneously)
    if (flock(fd, LOCK_SH) != 0) {
        perror("[PERSISTENCE] flock(LOCK_SH) failed");
        close(fd);
        return -1;
    }

    // Use fdopen to get a FILE* for convenient fgets() line reading
    // We already have the lock via fd — fdopen wraps the same fd
    FILE *f = fdopen(fd, "r");
    if (!f) {
        perror("[PERSISTENCE] fdopen() failed");
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }

    int count = 0;
    char line[4096];

    while (fgets(line, sizeof(line), f)) {
        // Skip comment lines (start with '#') and blank lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Strip trailing newline
        line[strcspn(line, "\r\n")] = '\0';

        char key[512], value[4096];
        long expiry_raw;

        // Parse: "key value expiry_time"
        int parsed = sscanf(line, "%511s %4095s %ld", key, value, &expiry_raw);
        if (parsed != 3) {
            fprintf(stderr, "[PERSISTENCE] Skipping malformed line: %s\n", line);
            continue;
        }

        time_t expiry = (time_t)expiry_raw;

        // Skip keys that have already expired — no point loading them
        if (expiry != 0 && expiry <= time(NULL)) {
            printf("[PERSISTENCE] Skipping expired key: %s\n", key);
            continue;
        }

        // Insert into hash table
        ht_set(ht, key, value);

        // Restore the expiry time if one was set
        if (expiry != 0) {
            ht_set_expiry(ht, key, expiry);
        }

        count++;
    }

    // fclose() closes the underlying fd too — and releases the flock
    // because the file descriptor is closed
    fclose(f);

    printf("[PERSISTENCE] Loaded %d key(s) from %s\n", count, filepath);
    return count;
}

// =============================================================================
// BGSAVE WATCHER THREAD
//
// This function runs in a detached pthread inside the parent process.
// It blocks on read(pipe_read_fd, ...) until the child writes "DONE".
// When it gets "DONE", it logs the completion and exits.
//
// WHY A SEPARATE THREAD FOR THE WATCHER?
// If the parent blocked on pipe read directly in the main thread,
// no new client commands could be processed during the save.
// Running the watcher in its own pthread keeps the parent fully responsive.
//
// OS Concept 6: IPC — reading from the pipe that the child wrote to
// =============================================================================
typedef struct {
    int pipe_read_fd;  // The read end of the pipe from the BGSAVE child
} BgsaveWatcherArg;

static void *bgsave_watcher_thread(void *arg) {
    BgsaveWatcherArg *warg = (BgsaveWatcherArg *)arg;
    int pipe_read_fd = warg->pipe_read_fd;
    free(warg);  // We own this heap allocation

    // Block here until the child writes to the pipe
    char msg[16];
    memset(msg, 0, sizeof(msg));
    ssize_t bytes = read(pipe_read_fd, msg, sizeof(msg) - 1);

    if (bytes > 0) {
        msg[bytes] = '\0';
        printf("[BGSAVE] Child process completed: %s\n", msg);
    } else {
        printf("[BGSAVE] Child pipe closed unexpectedly\n");
    }

    close(pipe_read_fd);
    return NULL;
}

// =============================================================================
// bgsave_async — Non-blocking background save via fork() + pipe()
//
// OS Concept 6: Inter-Process Communication (IPC)
//
// Flow:
//   1. Create a pipe (two file descriptors: pipe[0]=read, pipe[1]=write)
//   2. fork() — creates an identical child process
//   3. CHILD:  closes pipe[0] (read end, it only writes)
//              takes a snapshot of db and saves to dump.rdb
//              writes "DONE" to pipe[1] to notify parent
//              exit(0)
//   4. PARENT: closes pipe[1] (write end, it only reads)
//              spawns a watcher pthread that reads from pipe[0]
//              returns "+OK Background saving started" IMMEDIATELY
//              continues serving other clients while child saves
//
// WHY fork() SPECIFICALLY?
//   fork() creates a copy-on-write snapshot of the parent's address space.
//   The child sees the exact state of db at the moment of fork, even if
//   the parent continues modifying it. This is how real Redis BGSAVE works —
//   it exploits Linux's copy-on-write to get a consistent snapshot without
//   stopping the world.
//
// NOTE: In this project, we lock db_mutex BEFORE forking to get a clean snapshot,
//       then unlock in the parent. The child inherits the locked mutex state and
//       calls save_snapshot (which uses flock, not db_mutex) to write the file.
// =============================================================================
void bgsave_async(HashTable *ht, char *response_buf, size_t response_size) {

    // Step 1: Create the pipe
    // pipe[0] = read end (parent reads from this)
    // pipe[1] = write end (child writes to this)
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("[BGSAVE] pipe() failed");
        snprintf(response_buf, response_size, "-ERR BGSAVE failed (pipe)\r\n");
        return;
    }

    // Step 2: Lock db_mutex so the child gets a consistent snapshot
    // The child will inherit this locked state
    pthread_mutex_lock(&db_mutex);

    // Step 3: Fork
    pid_t pid = fork();

    if (pid < 0) {
        // fork() failed — clean up and report error
        pthread_mutex_unlock(&db_mutex);
        close(pipefd[0]);
        close(pipefd[1]);
        perror("[BGSAVE] fork() failed");
        snprintf(response_buf, response_size, "-ERR BGSAVE failed (fork)\r\n");
        return;
    }

    if (pid == 0) {
        // ====================================================================
        // CHILD PROCESS
        // After fork(), this is a separate process with its own memory space.
        // It has a copy of db (copy-on-write — no malloc needed).
        // It must exit() when done — it must NOT return to the server loop.
        // ====================================================================

        // Close the read end — child only writes to the pipe
        close(pipefd[0]);

        // The child inherited db_mutex in a locked state.
        // We need to unlock it here because mutexes are per-process —
        // the parent's lock does not block the child's own operations.
        // We also call pthread_mutex_unlock to reset the state cleanly.
        pthread_mutex_unlock(&db_mutex);

        // Save to disk — save_snapshot uses flock() internally for file safety
        int saved = save_snapshot(ht, DUMP_FILE);

        // Notify parent via pipe
        // OS Concept 6: IPC — write to pipe
        if (saved >= 0) {
            char done_msg[32];
            snprintf(done_msg, sizeof(done_msg), "DONE:%d", saved);
            write(pipefd[1], done_msg, strlen(done_msg));
        } else {
            write(pipefd[1], "ERROR", 5);
        }

        close(pipefd[1]);

        // Child exits — critical that it never reaches the accept() loop
        exit(0);

    } else {
        // ====================================================================
        // PARENT PROCESS
        // Unlock db_mutex immediately so client threads can continue
        // The child already has its snapshot — parent modifications don't matter
        // ====================================================================

        pthread_mutex_unlock(&db_mutex);

        // Close the write end — parent only reads from pipe
        close(pipefd[1]);

        // Reap zombie children automatically
        // SIGCHLD SIG_IGN tells the kernel to auto-reap children
        // so we don't accumulate zombie processes
        signal(SIGCHLD, SIG_IGN);

        // Spawn a watcher thread to read from the pipe when child finishes
        // This is non-blocking — the watcher runs independently
        // OS Concept 6: IPC — watcher reads from pipe that child writes to
        BgsaveWatcherArg *warg = malloc(sizeof(BgsaveWatcherArg));
        if (warg) {
            warg->pipe_read_fd = pipefd[0];
            pthread_t watcher_tid;
            if (pthread_create(&watcher_tid, NULL, bgsave_watcher_thread, warg) == 0) {
                pthread_detach(watcher_tid);
            } else {
                // If watcher thread fails, just close the fd — non-fatal
                free(warg);
                close(pipefd[0]);
            }
        } else {
            close(pipefd[0]);
        }

        printf("[BGSAVE] Background save started (child PID=%d)\n", pid);

        // Return immediately — parent continues serving clients
        snprintf(response_buf, response_size,
                 "+OK Background saving started (child PID=%d)\r\n", pid);
    }
}