#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "auth.h"

User user_table[MAX_USERS];
int  user_count = 0;

/*
    Reads users.conf line by line
    username password role_number
 */

int load_users_from_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[AUTH] ERROR: Cannot open '%s'\n", filepath);
        fprintf(stderr, "[AUTH] Create a users.conf file in the project directory.\n");
        return 0;
    }

    char line[256];
    user_count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Remove trailing newline
        line[strcspn(line, "\r\n")] = '\0';

        char uname[MAX_USERNAME_LEN];
        char upass[MAX_PASSWORD_LEN];
        int  urole;

        // Parse: "username password role"
        int parsed = sscanf(line, "%63s %63s %d", uname, upass, &urole);

        if (parsed != 3) {
            fprintf(stderr, "[AUTH] WARNING: Skipping malformed line: '%s'\n", line);
            continue;
        }

        if (urole < 0 || urole > 2) {
            fprintf(stderr, "[AUTH] WARNING: Invalid role %d for user '%s'. Skipping.\n",
                    urole, uname);
            continue;
        }

        if (user_count >= MAX_USERS) {
            fprintf(stderr, "[AUTH] WARNING: Max users (%d) reached. Ignoring remaining.\n",
                    MAX_USERS);
            break;
        }

        strncpy(user_table[user_count].username, uname, MAX_USERNAME_LEN - 1);
        strncpy(user_table[user_count].password, upass, MAX_PASSWORD_LEN - 1);
        user_table[user_count].role = (Role)urole;
        user_count++;
    }

    fclose(f);

    if (user_count == 0) {
        fprintf(stderr, "[AUTH] ERROR: No valid users found in '%s'\n", filepath);
        return 0;
    }

    printf("[AUTH] Loaded %d user(s) from '%s'\n", user_count, filepath);
    return 1;
}

/*
 * Called when client sends: AUTH username password
 * Loops through in-memory user_table, finds match, sets session role.
 */
int authenticate(ClientSession *session, char *username, char *password) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(user_table[i].username, username) == 0 &&
            strcmp(user_table[i].password, password) == 0) {

            session->authenticated = 1;
            session->role = user_table[i].role;
            strncpy(session->username, username, MAX_USERNAME_LEN - 1);
            return 1;  // Success
        }
    }
    return 0;  // Invalid credentials
}

/*
  Checks if the session's role is allowed to perform an operation.
  ADMIN (0) can do everything.
  WRITER (1) can do everything except ADMIN-only commands.
  READER (2) can only do read operations.
 */
int has_permission(ClientSession *session, Role required_role) {
    if (!session->authenticated) return 0;
    return session->role <= required_role;
}