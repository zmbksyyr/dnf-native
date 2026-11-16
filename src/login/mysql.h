#ifndef DNF_MYSQL_H
#define DNF_MYSQL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void    *sock;      /* SOCKET (kept void* to avoid winsock header here) */
    int      connected;
    char     error[512];
} MyConn;

/* Connect and authenticate using mysql_native_password.
   Returns 0 on success, -1 on failure (error text in conn->error). */
int my_connect(MyConn *c, const char *host, unsigned short port,
               const char *user, const char *password);

void my_close(MyConn *c);

/* Execute a SQL statement that returns 0 or more rows. On success returns 0
   and, if row_out != NULL, stores a heap-allocated first-column string of the
   first row (caller must free). If no rows, *row_out is set to NULL.
   Returns -1 on error (error text in conn->error). */
int my_query_one(MyConn *c, const char *sql, char **row_out);

/* Execute a statement that returns no rows (INSERT/UPDATE). 0 = OK, -1 = error. */
int my_exec(MyConn *c, const char *sql);

/* Escape a string for use inside SQL single quotes. out must be at least
   2*strlen(in)+1 bytes. Returns length written (excluding NUL). */
size_t my_escape_string(const char *in, char *out);

#ifdef __cplusplus
}
#endif

#endif /* DNF_MYSQL_H */
