#include "mysql.h"
#include "crypto.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* MySQL capability flags we request */
#define CLIENT_LONG_PASSWORD      0x00000001u
#define CLIENT_FOUND_ROWS         0x00000002u
#define CLIENT_LONG_FLAG          0x00000004u
#define CLIENT_CONNECT_WITH_DB    0x00000008u
#define CLIENT_PROTOCOL_41        0x00000200u
#define CLIENT_TRANSACTIONS       0x00002000u
#define CLIENT_SECURE_CONNECTION  0x00008000u
#define CLIENT_PLUGIN_AUTH        0x00080000u
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA 0x00200000u

#define CAPS (CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 | \
              CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION | \
              CLIENT_PLUGIN_AUTH | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)

static void seterr(MyConn *c, const char *msg) {
    snprintf(c->error, sizeof(c->error), "%s", msg);
}
static void seterrf(MyConn *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, ap);
    va_end(ap);
}

static int recv_all(SOCKET s, char *buf, int len) {
    int got = 0;
    while (got < len) {
        int r = recv(s, buf + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}
static int send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

/* Read one MySQL packet. Returns malloc'd payload and its length; seq updated. */
static int read_packet(MyConn *c, char **payload, unsigned long *plen, unsigned char *seq) {
    SOCKET s = (SOCKET)c->sock;
    unsigned char hdr[4];
    if (recv_all(s, (char*)hdr, 4) != 0) { seterr(c, "recv packet header failed"); return -1; }
    unsigned long len = (unsigned long)hdr[0] | ((unsigned long)hdr[1]<<8) | ((unsigned long)hdr[2]<<16);
    *seq = hdr[3];
    char *buf = (char*)malloc((size_t)len + 1);
    if (!buf) { seterr(c, "out of memory"); return -1; }
    if (len && recv_all(s, buf, (int)len) != 0) { free(buf); seterr(c, "recv packet body failed"); return -1; }
    buf[len] = '\0';
    *payload = buf; *plen = len;
    return 0;
}
static int write_packet(MyConn *c, const char *payload, unsigned long len, unsigned char seq) {
    SOCKET s = (SOCKET)c->sock;
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(len & 0xff);
    hdr[1] = (unsigned char)((len>>8) & 0xff);
    hdr[2] = (unsigned char)((len>>16) & 0xff);
    hdr[3] = seq;
    if (send_all(s, (char*)hdr, 4) != 0) { seterr(c, "send packet header failed"); return -1; }
    if (len && send_all(s, payload, (int)len) != 0) { seterr(c, "send packet body failed"); return -1; }
    return 0;
}

/* length-encoded integer */
static unsigned long long read_lenenc(const char *p, size_t avail, size_t *used) {
    unsigned char b = (unsigned char)p[0];
    if (b < 0xfb) { *used = 1; return b; }
    if (b == 0xfc) { *used = 3; if(avail>=3) return (unsigned long long)(unsigned char)p[1] | ((unsigned long long)(unsigned char)p[2]<<8); return 0; }
    if (b == 0xfd) { *used = 4; if(avail>=4) return (unsigned long long)(unsigned char)p[1] | ((unsigned long long)(unsigned char)p[2]<<8) | ((unsigned long long)(unsigned char)p[3]<<16); return 0; }
    if (b == 0xfe) { *used = 9; if(avail>=9){ unsigned long long v=0; for(int i=1;i<=8;i++) v |= ((unsigned long long)(unsigned char)p[i])<<(8*(i-1)); return v;} return 0; }
    *used = 1; return 0; /* 0xfb = NULL */
}

/* Parse an ERR packet, filling conn->error. Payload starts with 0xff. */
static void parse_err(MyConn *c, const char *buf, unsigned long len) {
    const char *msg = "unknown error";
    if (len >= 3) {
        /* 0xff, 2-byte error code, '#' + 5-char sqlstate (if present), message */
        size_t off = 3;
        if (len > off && buf[off] == '#') off += 6;
        msg = buf + off;
    }
    seterrf(c, "MySQL error: %s", msg);
}

/* Compute mysql_native_password scramble:
   SCRAMBLE = SHA1(pass) XOR SHA1(salt + SHA1(SHA1(pass))) */
static void native_scramble(const char *pass, const unsigned char salt[20],
                            unsigned char out[20]) {
    uint8_t sha1_pass[20], sha1_sha1_pass[20], sha1_salt_msg[20];
    sha1_hash((const uint8_t*)pass, strlen(pass), sha1_pass);
    sha1_hash(sha1_pass, 20, sha1_sha1_pass);
    uint8_t msg[40];
    memcpy(msg, salt, 20);
    memcpy(msg + 20, sha1_sha1_pass, 20);
    sha1_hash(msg, 40, sha1_salt_msg);
    for (int i = 0; i < 20; i++) out[i] = sha1_pass[i] ^ sha1_salt_msg[i];
}

int my_connect(MyConn *c, const char *host, unsigned short port,
               const char *user, const char *password) {
    static int wsa_inited = 0;
    WSADATA wsa;
    memset(c, 0, sizeof(*c));
    if (!wsa_inited) { if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { seterr(c,"WSAStartup failed"); return -1; } wsa_inited = 1; }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) { seterrf(c, "getaddrinfo failed for %s:%u", host, port); return -1; }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); seterr(c, "socket failed"); return -1; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { closesocket(s); freeaddrinfo(res); seterrf(c, "connect to %s:%u failed", host, port); return -1; }
    freeaddrinfo(res);
    c->sock = (void*)s;

    /* --- read initial handshake --- */
    char *buf = NULL; unsigned long len = 0; unsigned char seq = 0;
    if (read_packet(c, &buf, &len, &seq) != 0) { my_close(c); return -1; }
    if (len < 1 || (unsigned char)buf[0] != 10) { free(buf); my_close(c); seterr(c, "unsupported MySQL protocol version"); return -1; }
    unsigned char salt[20] = {0};
    size_t off = 1;
    while (off < len && buf[off] != '\0') off++;
    off++; /* skip NUL server version */
    if (off + 4 > len) { free(buf); my_close(c); seterr(c, "bad handshake"); return -1; }
    off += 4; /* connection id */
    if (off + 8 > len) { free(buf); my_close(c); seterr(c, "bad handshake salt1"); return -1; }
    memcpy(salt, buf + off, 8); off += 8;
    off++; /* filler */
    if (off + 2 > len) { free(buf); my_close(c); seterr(c, "bad handshake caps"); return -1; }
    off += 2; /* capability lower */
    if (off >= len) { free(buf); my_close(c); seterr(c, "bad handshake charset"); return -1; }
    off++; /* charset */
    off += 2; /* status */
    off += 2; /* capability upper */
    unsigned char authlen = 21;
    if (off < len) authlen = (unsigned char)buf[off];
    off += 1; /* auth plugin data len */
    off += 10; /* reserved */
    int salt2len = authlen > 8 ? authlen - 8 : 13;
    if (salt2len > 12) salt2len = 12;
    if (off + salt2len <= len) memcpy(salt + 8, buf + off, salt2len);
    free(buf);

    /* --- send handshake response --- */
    unsigned char scramble[20];
    native_scramble(password, salt, scramble);
    size_t ulen = strlen(user);
    size_t cap_len = 4 + 4 + 1 + 23 + ulen + 1 + 1 + 20 + strlen("mysql_native_password") + 1;
    char *resp = (char*)calloc(1, cap_len);
    size_t p = 0;
    uint32_t caps = CAPS;
    if (!(caps & CLIENT_PLUGIN_AUTH)) caps |= CLIENT_PLUGIN_AUTH; /* ensure */
    resp[p++] = (char)(caps & 0xff);
    resp[p++] = (char)((caps>>8) & 0xff);
    resp[p++] = (char)((caps>>16) & 0xff);
    resp[p++] = (char)((caps>>24) & 0xff);
    uint32_t maxpkt = 0x01000000u; /* 16MB */
    resp[p++] = (char)(maxpkt & 0xff); resp[p++] = (char)((maxpkt>>8)&0xff);
    resp[p++] = (char)((maxpkt>>16)&0xff); resp[p++] = (char)((maxpkt>>24)&0xff);
    resp[p++] = 33; /* utf8mb4 charset */
    memset(resp + p, 0, 23); p += 23;
    memcpy(resp + p, user, ulen); p += ulen; resp[p++] = 0;
    resp[p++] = 20; /* auth response length (1 byte, fits) */
    memcpy(resp + p, scramble, 20); p += 20;
    memcpy(resp + p, "mysql_native_password", strlen("mysql_native_password")); p += strlen("mysql_native_password"); resp[p++] = 0;
    if (write_packet(c, resp, (unsigned long)p, seq + 1) != 0) { free(resp); my_close(c); return -1; }
    free(resp);

    /* --- read auth result --- */
    if (read_packet(c, &buf, &len, &seq) != 0) { my_close(c); return -1; }
    unsigned char first = len ? (unsigned char)buf[0] : 0xff;
    if (first == 0xff) { parse_err(c, buf, len); free(buf); my_close(c); return -1; }
    /* AuthSwitchRequest: 0xfe + plugin name + new salt. Handle it. */
    if (first == 0xfe && len > 1) {
        const char *plugin = buf + 1;
        size_t plen = 0; while (1 + plen < len && plugin[plen] != '\0') plen++;
        if (strcmp(plugin, "mysql_native_password") != 0) {
            seterrf(c, "unsupported auth plugin: %s", plugin); free(buf); my_close(c); return -1;
        }
        const unsigned char *nsalt = (const unsigned char*)plugin + plen + 1;
        unsigned char swsalt[20] = {0};
        size_t rem = len - (1 + plen + 1);
        if (rem > 20) rem = 20;
        memcpy(swsalt, nsalt, rem);
        native_scramble(password, swsalt, scramble);
        free(buf);
        if (write_packet(c, (char*)scramble, 20, seq + 1) != 0) { my_close(c); return -1; }
        if (read_packet(c, &buf, &len, &seq) != 0) { my_close(c); return -1; }
        first = len ? (unsigned char)buf[0] : 0xff;
        if (first == 0xff) { parse_err(c, buf, len); free(buf); my_close(c); return -1; }
    }
    if (first != 0x00) { free(buf); my_close(c); seterr(c, "unexpected authentication response"); return -1; }
    free(buf);
    c->connected = 1;
    return 0;
}

void my_close(MyConn *c) {
    if (c && c->sock && c->sock != (void*)INVALID_SOCKET) {
        closesocket((SOCKET)c->sock);
        c->sock = NULL;
    }
    c->connected = 0;
}

/* Internal: send COM_QUERY and read a result set.
   mode 0 = want first row value, mode 1 = exec (ignore rows). */
static int do_query(MyConn *c, const char *sql, char **row_out, int mode) {
    if (!c->connected) { seterr(c, "not connected"); return -1; }
    char *buf = NULL; unsigned long len = 0; unsigned char seq = 0;
    size_t sqlen = strlen(sql);
    char *q = (char*)malloc(sqlen + 1);
    q[0] = 0x03; memcpy(q + 1, sql, sqlen);
    if (write_packet(c, q, (unsigned long)(sqlen + 1), 0) != 0) { free(q); return -1; }
    free(q);

    if (row_out) *row_out = NULL;
    if (read_packet(c, &buf, &len, &seq) != 0) return -1;
    unsigned char first = len ? (unsigned char)buf[0] : 0xff;
    if (first == 0xff) { parse_err(c, buf, len); free(buf); return -1; }
    if (first == 0x00) { free(buf); return 0; } /* OK packet, no result set */

    /* result set: first packet is column count (lenenc int) */
    size_t used = 0;
    unsigned long long ncols = read_lenenc(buf, len, &used);
    free(buf);

    /* read and discard column definition packets */
    for (unsigned long long i = 0; i < ncols; i++) {
        if (read_packet(c, &buf, &len, &seq) != 0) return -1;
        free(buf);
    }
    /* read EOF after column defs */
    if (read_packet(c, &buf, &len, &seq) != 0) return -1;
    free(buf);

    /* read rows until EOF (0xfe with len<9) */
    int got_value = 0;
    while (1) {
        if (read_packet(c, &buf, &len, &seq) != 0) return -1;
        if (len && len < 9 && (unsigned char)buf[0] == 0xfe) { free(buf); break; } /* EOF */
        if (len && (unsigned char)buf[0] == 0xff) { parse_err(c, buf, len); free(buf); return -1; }
        /* row: sequence of length-encoded strings */
        if (mode == 0 && row_out && !got_value && len > 0) {
            size_t u = 0;
            unsigned long long vlen = read_lenenc(buf, len, &u);
            if (vlen != 0xfbULL && vlen != 0xfcULL && vlen != 0xfdULL && vlen != 0xfeULL) {
                if (u + vlen <= len) {
                    char *val = (char*)malloc((size_t)vlen + 1);
                    memcpy(val, buf + u, (size_t)vlen);
                    val[vlen] = '\0';
                    *row_out = val;
                    got_value = 1;
                }
            }
        }
        free(buf);
    }
    return 0;
}

int my_query_one(MyConn *c, const char *sql, char **row_out) {
    return do_query(c, sql, row_out, 0);
}
int my_exec(MyConn *c, const char *sql) {
    return do_query(c, sql, NULL, 1);
}

size_t my_escape_string(const char *in, char *out) {
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        char ch = *p;
        switch (ch) {
            case '\0': out[o++]='\\'; out[o++]='0'; break;
            case '\n': out[o++]='\\'; out[o++]='n'; break;
            case '\r': out[o++]='\\'; out[o++]='r'; break;
            case '\\': out[o++]='\\'; out[o++]='\\'; break;
            case '\'': out[o++]='\\'; out[o++]='\''; break;
            case '"':  out[o++]='\\'; out[o++]='"'; break;
            case 0x1a: out[o++]='\\'; out[o++]='Z'; break;
            default: out[o++]=ch; break;
        }
    }
    out[o] = '\0';
    return o;
}
