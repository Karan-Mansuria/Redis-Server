/*
  hastable.h - Hash Table Interface for Mini Redis Server
 
  Each Entry has key, value and expiry time.
  We are using separate chaining for collision handling.
  
   Only the Logic of the HashTable is used and the locking is handled in other file.
 */

#ifndef HASTABLE_H
#define HASTABLE_H

#include <stdio.h>
#include <time.h>

#define TABLE_SIZE 256


typedef struct Entry {
    char *key;
    char *value;
    time_t expiry_time;     // timestamp 0 = no expiry 
    struct Entry *next;     // Next node in chain (collision handling) 
} Entry;

/*
  The hash table: a fixed size array of bucket pointers,
  each pointing to the head of a linked list of Entry nodes.
 */

typedef struct {
    Entry *buckets[TABLE_SIZE];
    int size;               // Current number of keys stored  
} HashTable;

//   Core Operations   

HashTable *ht_create(void);
void       ht_destroy(HashTable *ht);
int        ht_set(HashTable *ht, char *key, char *value);
char      *ht_get(HashTable *ht, char *key);
int        ht_delete(HashTable *ht, char *key);
int        ht_exists(HashTable *ht, char *key);

//  Expiry Operations   

void       ht_set_expiry(HashTable *ht, char *key, time_t expiry);
time_t     ht_get_expiry(HashTable *ht, char *key);
void       ht_purge_expired(HashTable *ht);      // Delete all expired entries

//  Persistence Operations   

int        ht_save_to_file(HashTable *ht, int fd);
int        ht_load_from_file(HashTable *ht, FILE *f);

#endif //  HASTABLE_H  