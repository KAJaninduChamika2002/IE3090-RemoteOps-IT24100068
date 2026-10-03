/*
 * agent_068.c - RemoteOps Agent
 * IE3090 Network Programming - IT24100068
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

#define REG_NO       "IT24100068"
#define AGENT_PORT   9410
#define SID_TAG      "SID:8600"
#define AUTH_TOKEN   "OPS-0068"
#define LOG_FILE     "remoteops_IT24100068.log"
#define STORAGE_ROOT "./agentfiles"
#define STORAGE_DIR  "./agentfiles/IT24100068"

#define BACKLOG      16
#define BUF_SIZE     8192
#define MAX_LINE     1024
#define MAX_RESP     32768
#define LOG_PREVIEW  200

typedef struct {
    int    fd;
    char   ip[INET_ADDRSTRLEN];
    int    port;
    int    authenticated;
    char   buf[BUF_SIZE];
    size_t buf_len;
} session_t;

/* client only picks a name, the actual command is fixed here */
static const struct {
    const char *name;
    const char *command;
} EXEC_WHITELIST[] = {
    { "DATE",     "date 2>&1"      },
    { "UPTIME",   "uptime -p 2>&1" },
    { "DISKFREE", "df -h / 2>&1"   },
    { "HOSTNAME", "uname -n 2>&1"  },
    { "WHOAMI",   "whoami 2>&1"    },
};
#define EXEC_COUNT (sizeof EXEC_WHITELIST / sizeof EXEC_WHITELIST[0])

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_event(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, msg);
        fclose(f);
    }
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_response(session_t *s, const char *fmt, ...)
{
    char body[MAX_RESP];
    char line[MAX_RESP + 32];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    int len = snprintf(line, sizeof line, "%s %s\n", body, SID_TAG);
    if (len < 0) return -1;
    if ((size_t)len >= sizeof line) len = sizeof line - 1;

    log_event("TX %s:%d -> %.*s%s %s", s->ip, s->port, LOG_PREVIEW, body,
              strlen(body) > LOG_PREVIEW ? "..." : "", SID_TAG);
    return send_all(s->fd, line, (size_t)len);
}

/* returns 1 = line, 0 = closed, -1 = error, -2 = too long */
static int read_line(session_t *s, char *out, size_t out_size)
{
    for (;;) {
        char *nl = memchr(s->buf, '\n', s->buf_len);
        if (nl) {
            size_t line_len = (size_t)(nl - s->buf);
            size_t copy = line_len < out_size - 1 ? line_len : out_size - 1;
            memcpy(out, s->buf, copy);
            out[copy] = '\0';
            if (copy > 0 && out[copy - 1] == '\r')
                out[copy - 1] = '\0';

            size_t used = line_len + 1;
            memmove(s->buf, s->buf + used, s->buf_len - used);
            s->buf_len -= used;
            return 1;
        }

        if (s->buf_len >= MAX_LINE) {
            s->buf_len = 0;
            return -2;
        }

        ssize_t n = recv(s->fd, s->buf + s->buf_len, BUF_SIZE - s->buf_len, 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        s->buf_len += (size_t)n;
    }
}

static void get_sysinfo(double *cpu_load, long *mem_used_mb, long *uptime_sec)
{
    char line[256];
    *cpu_load = 0.0;
    *mem_used_mb = 0;
    *uptime_sec = 0;

    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf", cpu_load) != 1) *cpu_load = 0.0;
        fclose(f);
    }

    long total_kb = -1, avail_kb = -1;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld kB", &total_kb);
            sscanf(line, "MemAvailable: %ld kB", &avail_kb);
        }
        fclose(f);
    }
    if (total_kb > 0 && avail_kb >= 0)
        *mem_used_mb = (total_kb - avail_kb) / 1024;

    double up = 0.0;
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) == 1) *uptime_sec = (long)up;
        fclose(f);
    }
}

/* protocol is one line per reply, so newlines become " | " */
static void run_oneline(const char *command, char *out, size_t out_size)
{
    size_t pos = 0;
    int pending_sep = 0;
    out[0] = '\0';

    FILE *p = popen(command, "r");
    if (!p) {
        snprintf(out, out_size, "(failed to run command)");
        return;
    }

    int c;
    while ((c = fgetc(p)) != EOF) {
        if (c == '\r') continue;
        if (c == '\n') { pending_sep = 1; continue; }
        if (c == '\t') c = ' ';
        if (pending_sep && pos > 0) {
            if (pos + 3 >= out_size) break;
            memcpy(out + pos, " | ", 3);
            pos += 3;
        }
        pending_sep = 0;
        if (pos + 1 >= out_size) break;
        out[pos++] = (char)c;
    }
    out[pos] = '\0';
    pclose(p);

    if (pos == 0) snprintf(out, out_size, "(no output)");
}

static void build_proc_list(char *out, size_t out_size, int *count)
{
    size_t pos = 0;
    char line[512];
    *count = 0;
    out[0] = '\0';

    FILE *p = popen("ps -eo pid=,comm=", "r");
    if (!p) {
        snprintf(out, out_size, "(ps failed)");
        return;
    }

    while (fgets(line, sizeof line, p)) {
        int pid;
        char name[256];
        if (sscanf(line, " %d %255[^\n]", &pid, name) != 2) continue;
        for (char *q = name; *q; q++)
            if (*q == ' ' || *q == ',') *q = '_';

        char entry[300];
        int n = snprintf(entry, sizeof entry, "%s%d:%s", pos ? "," : "", pid, name);
        if (n < 0 || pos + (size_t)n + 4 >= out_size) {
            memcpy(out + pos, ",...", 5);
            pos += 4;
            break;
        }
        memcpy(out + pos, entry, (size_t)n + 1);
        pos += (size_t)n;
        (*count)++;
    }
    pclose(p);
}

static void cmd_sysinfo(session_t *s)
{
    double load;
    long mem_mb, up;
    get_sysinfo(&load, &mem_mb, &up);
    send_response(s, "OK SYSINFO %.2f %ld %ld", load, mem_mb, up);
}

static void cmd_listproc(session_t *s)
{
    char list[MAX_RESP - 64];
    int count;
    build_proc_list(list, sizeof list, &count);
    log_event("LISTPROC %s:%d -> %d processes", s->ip, s->port, count);
    send_response(s, "OK PROCS %s", list);
}

static void cmd_exec(session_t *s, const char *name, const char *extra)
{
    if (name == NULL || extra != NULL) {
        send_response(s, "ERR 006 BAD_SYNTAX");
        return;
    }
    for (size_t i = 0; i < EXEC_COUNT; i++) {
        if (strcmp(name, EXEC_WHITELIST[i].name) == 0) {
            char output[2048];
            run_oneline(EXEC_WHITELIST[i].command, output, sizeof output);
            log_event("EXEC %s by %s:%d", name, s->ip, s->port);
            send_response(s, "OK EXEC_RESULT %s", output);
            return;
        }
    }
    log_event("EXEC rejected '%s' from %s:%d", name, s->ip, s->port);
    send_response(s, "ERR 002 COMMAND_NOT_ALLOWED");
}

/* returns 1 when the connection should close */
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

    if (strcmp(cmd, "AUTH") == 0) {
        send_response(s, "ERR 007 ALREADY_AUTHENTICATED");
    } else if (strcmp(cmd, "SYSINFO") == 0) {
        cmd_sysinfo(s);
    } else if (strcmp(cmd, "LISTPROC") == 0) {
        cmd_listproc(s);
    } else if (strcmp(cmd, "EXEC") == 0) {
        char *name  = strtok_r(NULL, " ", &save);
        char *extra = strtok_r(NULL, " ", &save);
        cmd_exec(s, name, extra);
    } else if (strcmp(cmd, "QUIT") == 0) {
        send_response(s, "OK BYE");
        return 1;
    } else {
        send_response(s, "ERR 008 UNKNOWN_COMMAND");
    }
    return 0;
}

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

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    mkdir(STORAGE_ROOT, 0755);
    mkdir(STORAGE_DIR, 0755);

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
        pthread_detach(tid);
    }
}
