/*
   Implements a hash table with separate chaining
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "hashtable.h"

/* ----------------------------------------------------------------
    Internal helpers
   ---------------------------------------------------------------- */

/*
   Using a djb2 hash function - Used since it is simple, efficient for mapping strings to integers
*/
static unsigned int hash(const char *key)
{
    unsigned int h = 5381;     //emperically proven starting number that leads to lesser collisions 
    int c;
    int len = strlen(key);
    for (int i=0;i<len;i++) 
    {   
        c = key[i];
        h = 33*h + c; 
    }
    return h % TABLE_SIZE;
}

/*
  Allocate a new Entry node with copies of key and value
  expiry_time is initialised to 0 which means NO EXPIRATION
  Returns NULL on allocation failure 
 */
static Entry *entry_create(const char *key, const char *value)
{
    Entry *e = malloc(sizeof(Entry));
    if (!e)
        return NULL;

    e->key = strdup(key);           // copies the key
    e->value = strdup(value);       // copies the value
    if (!e->key || !e->value) 
    {
        free(e->key);
        free(e->value);
        free(e);
        return NULL;
    }

    e->expiry_time = 0;
    e->next = NULL;
    return e;
}

/*
  Free a single Entry and its owned strings.
 */
static void entry_destroy(Entry *e)
{
    if (!e)
        return;
    free(e->key);
    free(e->value);
    free(e);
}

/* ----------------------------------------------------------------
    Core Operations
 ---------------------------------------------------------------- */

HashTable *ht_create(void)
{
    HashTable *ht = malloc(sizeof(HashTable));
    if (!ht)
        return NULL;

    for (int i = 0; i < TABLE_SIZE; i++)
        ht->buckets[i] = NULL;

    ht->size = 0;
    return ht;
}

void ht_destroy(HashTable *ht)
{
    if (!ht)
        return;

    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *cur = ht->buckets[i];
        while (cur) {
            Entry *next = cur->next;
            entry_destroy(cur);
            cur = next;
        }
    }
    free(ht);
}

int ht_set(HashTable *ht, char *key, char *value)
{
    unsigned int idx = hash(key);
    Entry *cur = ht->buckets[idx];

    // Key already exists hence updating its value
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            char *new_value = strdup(value);
            if (!new_value)
                return -1;
            free(cur->value);
            cur->value = new_value;
            cur->expiry_time = 0;   // Reset expiry on update 
            return 0;
        }
        cur = cur->next;
    }

    // Key not found, adding the new key-value pair in the very front of the linked list
    Entry *new_entry = entry_create(key, value);
    if (!new_entry)
        return -1;

    new_entry->next = ht->buckets[idx];
    ht->buckets[idx] = new_entry;
    ht->size++;
    return 0;
}

char *ht_get(HashTable *ht, char *key)
{
    unsigned int idx = hash(key);
    Entry *cur = ht->buckets[idx];

    while (cur) {
        if (strcmp(cur->key, key) == 0)
            return cur->value;
        cur = cur->next;
    }
    return NULL;
}

int ht_delete(HashTable *ht, char *key)
{
    unsigned int idx = hash(key);
    Entry *cur  = ht->buckets[idx];
    Entry *prev = NULL;

    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            // Unlink from chain 
            Entry *next = cur->next;

            if (prev)
                prev->next = next;
            else
                ht->buckets[idx] = next;

            entry_destroy(cur);
            ht->size--;     // 1 key is reduced now
            return 1;
        }
        prev = cur;
        cur  = cur->next;
    }
    return 0;   // Key not found 
}

int ht_exists(HashTable *ht, char *key)
{
    unsigned int idx = hash(key);
    Entry *cur = ht->buckets[idx];

    while (cur) {
        if (strcmp(cur->key, key) == 0)
            return 1;
        cur = cur->next;
    }
    return 0;
}

/* ----------------------------------------------------------------
   Expiry Operations
  ---------------------------------------------------------------- */

void ht_set_expiry(HashTable *ht, char *key, time_t expiry)
{
    unsigned int idx = hash(key);
    Entry *cur = ht->buckets[idx];

    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            cur->expiry_time = expiry;
            return;
        }
        cur = cur->next;
    }
    // Key not found silent no-op
}

time_t ht_get_expiry(HashTable *ht, char *key)
{
    unsigned int idx = hash(key);
    Entry *cur = ht->buckets[idx];

    while (cur) {
        if (strcmp(cur->key, key) == 0)
            return cur->expiry_time;
        cur = cur->next;
    }
    return 0;
}

void ht_purge_expired(HashTable *ht)
{
    time_t now = time(NULL);

    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *cur  = ht->buckets[i];
        Entry *prev = NULL;

        while (cur) {
            Entry *next = cur->next;

            if (cur->expiry_time != 0 && cur->expiry_time <= now) {
                // This entry has expired - unlink and free it.
                if (prev)
                    prev->next = next;
                else
                    ht->buckets[i] = next;

                entry_destroy(cur);
                ht->size--;
                // prev stays the same
            } else {
                prev = cur;
            }
            cur = next;
        }
    }
}

/* ----------------------------------------------------------------
   Persistence Operations
 
   On-disk format (per entry):
     key    (NUL-terminated string)
     value  (NUL-terminated string)
     expiry (8 bytes)
 * ---------------------------------------------------------------- */

/*
  Helper: write exactly `len` bytes to fd, handling partial writes.
  Returns 0 on success, -1 on error.
 */

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0)
            return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

int ht_save_to_file(HashTable *ht, int fd)
{
    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *cur = ht->buckets[i];
        while (cur) {
            // Write key including its NUL terminator 
            size_t key_len = strlen(cur->key) + 1;
            if (write_all(fd, cur->key, key_len) < 0)
                return -1;

            // Write value including its NUL terminator 
            size_t val_len = strlen(cur->value) + 1;
            if (write_all(fd, cur->value, val_len) < 0)
                return -1;

            // Write expiry_time as 8 bytes in big-endian order 
            uint64_t raw = (uint64_t)cur->expiry_time; 
            unsigned char ebuf[8];
            ebuf[0] = (raw >> 56) & 0xFF;
            ebuf[1] = (raw >> 48) & 0xFF;
            ebuf[2] = (raw >> 40) & 0xFF;
            ebuf[3] = (raw >> 32) & 0xFF;
            ebuf[4] = (raw >> 24) & 0xFF;
            ebuf[5] = (raw >> 16) & 0xFF;
            ebuf[6] = (raw >>  8) & 0xFF;
            ebuf[7] = (raw      ) & 0xFF;
            if (write_all(fd, ebuf, 8) < 0)
                return -1;

            cur = cur->next;
        }
    }
    return 0;
}

/*
  Helper: read a NUL-terminated string from a FILE*
  Reads one character at a time until '\0' or EOF.
  Returns a heap-allocated string, or NULL on EOF/error
 */
static char *read_string(FILE *f)
{
    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
        buf[len++] = (char)ch;
        if (ch == '\0')
            return buf;   /* Success - string is already NUL-terminated */
    }

    /* Reached EOF before finding a NUL terminator */
    free(buf);
    return NULL;
}

int ht_load_from_file(HashTable *ht, FILE *f)
{
    while (1) {
        char *key = read_string(f);
        if (!key)
            break;   // Clean EOF - no more entries 

        char *value = read_string(f);
        if (!value) {
            free(key);
            return -1;   // Corrupt file - key without value 
        }

        /* Read 8-byte big-endian expiry time */
        unsigned char ebuf[8];
        if (fread(ebuf, 1, 8, f) != 8) {
            free(key);
            free(value);
            return -1;   // Corrupt file - missing expiry 
        }
        
        // Reconstructs the original 64-bit integer from the array of bytes
        uint64_t raw = ((uint64_t)ebuf[0] << 56) |
                        ((uint64_t)ebuf[1] << 48) |
                        ((uint64_t)ebuf[2] << 40) |
                        ((uint64_t)ebuf[3] << 32) |
                        ((uint64_t)ebuf[4] << 24) |
                        ((uint64_t)ebuf[5] << 16) |
                        ((uint64_t)ebuf[6] <<  8) |
                        ((uint64_t)ebuf[7]);
        time_t expiry = (time_t)raw;

        //Insert into hash table 
        if (ht_set(ht, key, value) < 0) {
            free(key);
            free(value);
            return -1;
        }

        // Set expiry if non-zero 
        if (expiry != 0)
            ht_set_expiry(ht, key, expiry);

        free(key);
        free(value);
    }
    return 0;
}
