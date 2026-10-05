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
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#define MAX_FILE_SIZE (10L * 1024 * 1024)
#define CHUNK        65536
#define MONITOR_INTERVAL 5

typedef struct {
    int    fd;
    char   ip[INET_ADDRSTRLEN];
    int    port;
    int    authenticated;
    char   buf[BUF_SIZE];
    size_t buf_len;

    pthread_t       mon_thread;
    pthread_mutex_t mon_lock;
    pthread_cond_t  mon_cond;
    int             mon_running;
    int             mon_port;
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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* takes leftover bytes from the line buffer first, then reads the socket */
static ssize_t read_bytes(session_t *s, char *dst, size_t want)
{
    if (s->buf_len > 0) {
        size_t n = s->buf_len < want ? s->buf_len : want;
        memcpy(dst, s->buf, n);
        memmove(s->buf, s->buf + n, s->buf_len - n);
        s->buf_len -= n;
        return (ssize_t)n;
    }
    for (;;) {
        ssize_t n = recv(s->fd, dst, want, 0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

static int discard_bytes(session_t *s, long long left)
{
    char tmp[CHUNK];
    while (left > 0) {
        ssize_t n = read_bytes(s, tmp, left < CHUNK ? (size_t)left : CHUNK);
        if (n <= 0) return -1;
        left -= n;
    }
    return 0;
}

static int write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += n;
        len  -= (size_t)n;
    }
    return 0;
}

/* no paths, no hidden files - keeps everything inside STORAGE_DIR */
static int valid_filename(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > 200) return 0;
    if (name[0] == '.') return 0;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p < 33 || *p > 126) return 0;
    return 1;
}

static int parse_size(const char *str, long long *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(str, &end, 10);
    if (errno || *end != '\0' || end == str || v < 0) return 0;
    *out = v;
    return 1;
}

/* returns -1 if the connection was lost during the transfer */
static int cmd_put(session_t *s, const char *name, const char *size_str, const char *extra)
{
    long long size;
    if (!name || !size_str || extra || !parse_size(size_str, &size)) {
        send_response(s, "ERR 006 BAD_SYNTAX");
        return 0;
    }

    int bad_name = !valid_filename(name);
    if (bad_name || size > MAX_FILE_SIZE) {
        log_event("PUT rejected '%s' (%lld bytes) from %s:%d - %s", name, size,
                  s->ip, s->port, bad_name ? "invalid filename" : "too large");
        if (discard_bytes(s, size) < 0) return -1;
        if (bad_name) send_response(s, "ERR 010 INVALID_FILENAME");
        else          send_response(s, "ERR 004 FILE_TOO_LARGE");
        return 0;
    }

    char tmp_path[512], final_path[512];
    snprintf(tmp_path, sizeof tmp_path, "%s/.upload_XXXXXX", STORAGE_DIR);
    snprintf(final_path, sizeof final_path, "%s/%s", STORAGE_DIR, name);

    int fd = mkstemp(tmp_path);
    if (fd >= 0) fchmod(fd, 0644);
    if (fd < 0) {
        log_event("PUT '%s' cannot create temp file: %s", name, strerror(errno));
        if (discard_bytes(s, size) < 0) return -1;
        send_response(s, "ERR 011 STORAGE_ERROR");
        return 0;
    }

    double start = now_sec();
    char chunk[CHUNK];
    long long left = size;
    int write_failed = 0;

    while (left > 0) {
        ssize_t n = read_bytes(s, chunk, left < CHUNK ? (size_t)left : CHUNK);
        if (n <= 0) {
            close(fd);
            unlink(tmp_path);
            log_event("PUT '%s' aborted after %lld of %lld bytes (client lost)",
                      name, size - left, size);
            return -1;
        }
        if (!write_failed && write_all(fd, chunk, (size_t)n) < 0)
            write_failed = 1;
        left -= n;
    }
    close(fd);

    if (write_failed || rename(tmp_path, final_path) < 0) {
        unlink(tmp_path);
        log_event("PUT '%s' failed to save: %s", name, strerror(errno));
        send_response(s, "ERR 011 STORAGE_ERROR");
        return 0;
    }

    double secs = now_sec() - start;
    log_event("PUT '%s' %lld bytes from %s:%d in %.3f s (%.0f bytes/s)", name, size,
              s->ip, s->port, secs, secs > 0 ? size / secs : 0.0);
    send_response(s, "OK FILE_RECEIVED %s", name);
    return 0;
}

static int cmd_get(session_t *s, const char *name, const char *extra)
{
    if (!name || extra) {
        send_response(s, "ERR 006 BAD_SYNTAX");
        return 0;
    }
    if (!valid_filename(name)) {
        send_response(s, "ERR 010 INVALID_FILENAME");
        return 0;
    }

    char path[512];
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, name);

    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        log_event("GET '%s' not found for %s:%d", name, s->ip, s->port);
        send_response(s, "ERR 005 FILE_NOT_FOUND");
        return 0;
    }

    long long size = st.st_size;
    if (send_response(s, "OK FILE_SEND %s %lld", name, size) < 0) {
        close(fd);
        return -1;
    }

    double start = now_sec();
    char chunk[CHUNK];
    long long left = size;
    while (left > 0) {
        ssize_t n = read(fd, chunk, left < CHUNK ? (size_t)left : CHUNK);
        if (n <= 0 || send_all(s->fd, chunk, (size_t)n) < 0) {
            close(fd);
            log_event("GET '%s' aborted after %lld of %lld bytes", name, size - left, size);
            return -1;
        }
        left -= n;
    }
    close(fd);

    double secs = now_sec() - start;
    log_event("GET '%s' %lld bytes to %s:%d in %.3f s (%.0f bytes/s)", name, size,
              s->ip, s->port, secs, secs > 0 ? size / secs : 0.0);
    return 0;
}

/* sends a SYSINFO datagram every MONITOR_INTERVAL seconds until stopped */
static void *monitor_thread(void *arg)
{
    session_t *s = arg;
    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) {
        log_event("MONITOR %s:%d cannot create UDP socket: %s", s->ip, s->port, strerror(errno));
        return NULL;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(s->mon_port);
    inet_pton(AF_INET, s->ip, &dest.sin_addr);

    long sent = 0;
    pthread_mutex_lock(&s->mon_lock);
    while (s->mon_running) {
        double load;
        long mem_mb, up;
        get_sysinfo(&load, &mem_mb, &up);

        char msg[128];
        int len = snprintf(msg, sizeof msg, "SYSINFO %.2f %ld %ld %s", load, mem_mb, up, SID_TAG);
        sendto(ufd, msg, (size_t)len, 0, (struct sockaddr *)&dest, sizeof dest);
        sent++;

        struct timespec wake;
        clock_gettime(CLOCK_REALTIME, &wake);
        wake.tv_sec += MONITOR_INTERVAL;
        while (s->mon_running &&
               pthread_cond_timedwait(&s->mon_cond, &s->mon_lock, &wake) != ETIMEDOUT)
            ;
    }
    pthread_mutex_unlock(&s->mon_lock);

    close(ufd);
    log_event("MONITOR %s:%d -> UDP %d stopped after %ld datagrams", s->ip, s->port, s->mon_port, sent);
    return NULL;
}

static int start_monitor(session_t *s, int udp_port)
{
    pthread_mutex_lock(&s->mon_lock);
    s->mon_running = 1;
    s->mon_port = udp_port;
    pthread_mutex_unlock(&s->mon_lock);

    if (pthread_create(&s->mon_thread, NULL, monitor_thread, s) != 0) {
        s->mon_running = 0;
        return -1;
    }
    return 0;
}

static void stop_monitor(session_t *s)
{
    pthread_mutex_lock(&s->mon_lock);
    int was_running = s->mon_running;
    s->mon_running = 0;
    pthread_cond_signal(&s->mon_cond);
    pthread_mutex_unlock(&s->mon_lock);

    if (was_running)
        pthread_join(s->mon_thread, NULL);
}

static void cmd_monitor(session_t *s, const char *action, const char *port_str, const char *extra)
{
    if (action && strcmp(action, "START") == 0 && port_str && !extra) {
        long long port;
        if (!parse_size(port_str, &port) || port < 1 || port > 65535) {
            send_response(s, "ERR 006 BAD_SYNTAX");
            return;
        }
        if (s->mon_running) {
            send_response(s, "ERR 012 ALREADY_MONITORING");
            return;
        }
        if (start_monitor(s, (int)port) < 0) {
            send_response(s, "ERR 014 MONITOR_FAILED");
            return;
        }
        log_event("MONITOR %s:%d -> UDP %s:%lld every %d s", s->ip, s->port, s->ip, port, MONITOR_INTERVAL);
        send_response(s, "OK MONITOR_STARTED");
    } else if (action && strcmp(action, "STOP") == 0 && !port_str) {
        if (!s->mon_running) {
            send_response(s, "ERR 013 NOT_MONITORING");
            return;
        }
        stop_monitor(s);
        send_response(s, "OK MONITOR_STOPPED");
    } else {
        send_response(s, "ERR 006 BAD_SYNTAX");
    }
}

/* returns 1 = QUIT, -1 = connection lost, 0 = carry on */
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
    } else if (strcmp(cmd, "PUT") == 0) {
        char *name  = strtok_r(NULL, " ", &save);
        char *size  = strtok_r(NULL, " ", &save);
        char *extra = strtok_r(NULL, " ", &save);
        return cmd_put(s, name, size, extra);
    } else if (strcmp(cmd, "GET") == 0) {
        char *name  = strtok_r(NULL, " ", &save);
        char *extra = strtok_r(NULL, " ", &save);
        return cmd_get(s, name, extra);
    } else if (strcmp(cmd, "MONITOR") == 0) {
        char *action = strtok_r(NULL, " ", &save);
        char *port   = strtok_r(NULL, " ", &save);
        char *extra  = strtok_r(NULL, " ", &save);
        cmd_monitor(s, action, port, extra);
    } else if (strcmp(cmd, "QUIT") == 0) {
        stop_monitor(s);
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
            if (quit < 0) {
                log_event("DISCONNECT %s:%d (ungraceful: lost during transfer)", s->ip, s->port);
                break;
            }
        } else if (r == -2) {
            send_response(s, "ERR 009 LINE_TOO_LONG");
        } else {
            log_event("DISCONNECT %s:%d (ungraceful: %s)", s->ip, s->port,
                      r == 0 ? "peer closed" : strerror(errno));
            break;
        }
    }

    if (quit > 0)
        log_event("DISCONNECT %s:%d (QUIT)", s->ip, s->port);

    stop_monitor(s);
    close(s->fd);
    pthread_mutex_destroy(&s->mon_lock);
    pthread_cond_destroy(&s->mon_cond);
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

        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);

        session_t *s = calloc(1, sizeof *s);
        if (!s) { close(cfd); continue; }
        s->fd = cfd;
        inet_ntop(AF_INET, &caddr.sin_addr, s->ip, sizeof s->ip);
        s->port = ntohs(caddr.sin_port);
        pthread_mutex_init(&s->mon_lock, NULL);
        pthread_cond_init(&s->mon_cond, NULL);

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
