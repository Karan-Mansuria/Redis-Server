#ifndef AUTH_H
#define AUTH_H

#define MAX_USERS 50
#define MAX_USERNAME_LEN 64
#define MAX_PASSWORD_LEN 64

typedef enum { ADMIN = 0, WRITER = 1, READER = 2 } Role;

// Single user entry (loaded from users.conf)
typedef struct {
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];
    Role role;
} User;

// Per-client session (one per connected client thread)
typedef struct {
    int  socket_fd;
    Role role;
    char username[MAX_USERNAME_LEN];
    int  authenticated;   // 0 = not yet, 1 = authenticated
} ClientSession;

// Global user table (populated at startup from users.conf)
extern User user_table[MAX_USERS];
extern int  user_count;

// Functions
int  load_users_from_file(const char *filepath);  // Returns 1 on success, 0 on fail
int  authenticate(ClientSession *session, char *username, char *password);
int  has_permission(ClientSession *session, Role required_role);

#endif