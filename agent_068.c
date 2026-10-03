/*
 * agent_068.c  —  RemoteOps Agent (TCP server)
 * IE3090 Network Programming — IT24100068
 *
 * Day 1 scope: listen on personalised port, one thread per client,
 * line framing (partial / multiple lines per recv), AUTH gate, QUIT,
 * timestamped logging, graceful + ungraceful disconnect handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---------- Personalised values (IT24100068) ---------- */
#define REG_NO       "IT24100068"
#define AGENT_PORT   9410                 /* 7000 + 2410            */
#define SID_TAG      "SID:8600"           /* "0068" reversed        */
#define AUTH_TOKEN   "OPS-0068"           /* "OPS-" + last 4 digits */
#define LOG_FILE     "remoteops_IT24100068.log"
#define STORAGE_ROOT "./agentfiles"
#define STORAGE_DIR  "./agentfiles/IT24100068"

/* ---------- General settings ---------- */
#define BACKLOG      16
#define BUF_SIZE     8192                 /* per-client receive buffer */
#define MAX_LINE     1024                 /* longest command line      */
#define MAX_RESP     4096                 /* longest response line     */

/* One session = one connected Controller */
typedef struct {
    int    fd;                    /* TCP socket for this client          */
    char   ip[INET_ADDRSTRLEN];   /* client IP as text                   */
    int    port;                  /* client TCP port                     */
    int    authenticated;         /* 0 until AUTH succeeds               */
    char   buf[BUF_SIZE];         /* bytes received but not yet used     */
    size_t buf_len;               /* how many bytes are in buf           */
} session_t;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Logging (thread-safe, timestamped) ---------- */
static void log_event(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    char msg[MAX_RESP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);          /* only one thread writes at a time */
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, msg);
        fclose(f);
    }
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* ---------- send_all: keep calling send() until every byte is out ---------- */
static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);   /* no SIGPIPE on dead peer */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---------- send_response: every reply gets " SID:8600\n" appended ---------- */
static int send_response(session_t *s, const char *fmt, ...)
{
    char body[MAX_RESP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    char line[MAX_RESP + 32];
    int len = snprintf(line, sizeof line, "%s %s\n", body, SID_TAG);
    if (len < 0) return -1;
    if ((size_t)len >= sizeof line) len = sizeof line - 1;

    log_event("TX %s:%d -> %s %s", s->ip, s->port, body, SID_TAG);
    return send_all(s->fd, line, (size_t)len);
}

/*
 * read_line: return ONE complete command line (without '\n').
 * TCP is a stream: one recv() may hold half a line or several lines,
 * so bytes are kept in s->buf until a '\n' arrives.
 * Returns: 1 = got a line, 0 = peer closed, -1 = socket error, -2 = line too long
 */
static int read_line(session_t *s, char *out, size_t out_size)
{
    for (;;) {
        /* 1. Is there already a full line in the buffer? */
        char *nl = memchr(s->buf, '\n', s->buf_len);
        if (nl) {
            size_t line_len = (size_t)(nl - s->buf);
            size_t copy = line_len < out_size - 1 ? line_len : out_size - 1;
            memcpy(out, s->buf, copy);
            out[copy] = '\0';
            if (copy > 0 && out[copy - 1] == '\r')      /* tolerate CRLF */
                out[copy - 1] = '\0';

            /* 2. Shift leftover bytes (next lines / file data) to the front */
            size_t used = line_len + 1;
            memmove(s->buf, s->buf + used, s->buf_len - used);
            s->buf_len -= used;
            return 1;
        }

        /* 3. Buffer full but still no '\n' -> line is too long */
        if (s->buf_len >= MAX_LINE) {
            s->buf_len = 0;
            return -2;
        }

        /* 4. Need more bytes from the network */
        ssize_t n = recv(s->fd, s->buf + s->buf_len, BUF_SIZE - s->buf_len, 0);
        if (n == 0) return 0;                 /* client closed */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;                        /* e.g. connection reset */
        }
        s->buf_len += (size_t)n;
    }
}

/*
 * handle_command: process one line.
 * Returns 0 = keep going, 1 = close the connection (QUIT).
 */
static int handle_command(session_t *s, char *line)
{
    log_event("RX %s:%d <- %s", s->ip, s->port,
              strncmp(line, "AUTH", 4) == 0 ? "AUTH ****" : line);

    char *save = NULL;
    char *cmd  = strtok_r(line, " ", &save);

    if (cmd == NULL) {
        send_response(s, "ERR 006 BAD_SYNTAX");
        return 0;
    }

    /* ----- Before AUTH: only AUTH is accepted ----- */
    if (!s->authenticated) {
        if (strcmp(cmd, "AUTH") == 0) {
            char *token = strtok_r(NULL, " ", &save);
            char *extra = strtok_r(NULL, " ", &save);
            if (token && !extra && strcmp(token, AUTH_TOKEN) == 0) {
                s->authenticated = 1;
                log_event("AUTH success for %s:%d", s->ip, s->port);
                send_response(s, "OK AUTHENTICATED");
            } else {
                log_event("AUTH failure for %s:%d", s->ip, s->port);
                send_response(s, "ERR 001 AUTH_FAILED");
            }
        } else {
            send_response(s, "ERR 003 NOT_AUTHENTICATED");
        }
        return 0;
    }

    /* ----- After AUTH ----- */
    if (strcmp(cmd, "AUTH") == 0) {
        send_response(s, "ERR 007 ALREADY_AUTHENTICATED");
    } else if (strcmp(cmd, "QUIT") == 0) {
        send_response(s, "OK BYE");
        return 1;
    } else {
        /* SYSINFO, LISTPROC, EXEC, PUT, GET, MONITOR are added on later days */
        send_response(s, "ERR 008 UNKNOWN_COMMAND");
    }
    return 0;
}

/* ---------- One thread per connected Controller ---------- */
static void *client_thread(void *arg)
{
    session_t *s = arg;
    char line[MAX_LINE];
    int quit = 0;

    log_event("CONNECT %s:%d", s->ip, s->port);

    while (!quit) {
        int r = read_line(s, line, sizeof line);
        if (r == 1) {
            quit = handle_command(s, line);
        } else if (r == -2) {
            send_response(s, "ERR 009 LINE_TOO_LONG");
        } else {
            /* r == 0 (closed without QUIT) or r == -1 (reset) */
            log_event("DISCONNECT %s:%d (ungraceful: %s)", s->ip, s->port,
                      r == 0 ? "peer closed" : strerror(errno));
            break;
        }
    }

    if (quit)
        log_event("DISCONNECT %s:%d (QUIT)", s->ip, s->port);

    close(s->fd);
    free(s);
    return NULL;
}

/* ---------- main: set up listener, accept forever ---------- */
int main(void)
{
    signal(SIGPIPE, SIG_IGN);                 /* dead clients must not kill the Agent */

    mkdir(STORAGE_ROOT, 0755);                /* ./agentfiles             */
    mkdir(STORAGE_DIR, 0755);                 /* ./agentfiles/IT24100068  */

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(AGENT_PORT);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, BACKLOG) < 0) { perror("listen"); return 1; }

    log_event("RemoteOps Agent (%s) listening on TCP port %d", REG_NO, AGENT_PORT);

    for (;;) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof caddr;
        int cfd = accept(lfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        session_t *s = calloc(1, sizeof *s);
        if (!s) { close(cfd); continue; }
        s->fd = cfd;
        inet_ntop(AF_INET, &caddr.sin_addr, s->ip, sizeof s->ip);
        s->port = ntohs(caddr.sin_port);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, s) != 0) {
            perror("pthread_create");
            close(cfd);
            free(s);
            continue;
        }
        pthread_detach(tid);                  /* thread cleans itself up */
    }
}
