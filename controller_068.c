/*
 * controller_068.c - RemoteOps Controller
 * IE3090 Network Programming - IT24100068
 *
 * usage: ./controller_068 [agent_ip] [port]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <libgen.h>
#include <sys/stat.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define AGENT_PORT   9410
#define SID_TAG      "SID:8600"

#define BUF_SIZE     65536
#define MAX_INPUT    1024
#define SHOW_PROCS   30
#define CHUNK        65536
#define DOWNLOAD_DIR "downloads"

typedef struct {
    int    fd;
    char   buf[BUF_SIZE];
    size_t buf_len;
} conn_t;

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

static int read_line(conn_t *c, char *out, size_t out_size)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->buf_len);
        if (nl) {
            size_t line_len = (size_t)(nl - c->buf);
            size_t copy = line_len < out_size - 1 ? line_len : out_size - 1;
            memcpy(out, c->buf, copy);
            out[copy] = '\0';

            size_t used = line_len + 1;
            memmove(c->buf, c->buf + used, c->buf_len - used);
            c->buf_len -= used;
            return 1;
        }
        if (c->buf_len >= BUF_SIZE) {
            c->buf_len = 0;
            return -2;
        }
        ssize_t n = recv(c->fd, c->buf + c->buf_len, BUF_SIZE - c->buf_len, 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        c->buf_len += (size_t)n;
    }
}

static int connect_to_agent(const char *host, int port)
{
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { perror("socket"); freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static void show_sysinfo(const char *line)
{
    double load;
    long mem, up;
    if (sscanf(line, "OK SYSINFO %lf %ld %ld", &load, &mem, &up) == 3) {
        printf("   CPU load (1 min): %.2f | Memory used: %ld MB | Uptime: %ldd %02ld:%02ld:%02ld\n",
               load, mem, up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60);
    }
}

static void show_procs(char *line)
{
    char *list = line + strlen("OK PROCS ");
    char *sid = strstr(list, " " SID_TAG);
    if (sid) *sid = '\0';

    int count = 1;
    for (char *p = list; *p; p++)
        if (*p == ',') count++;
    printf("<< OK PROCS [%d entries] %s\n", count, SID_TAG);

    int col = 0;
    char *save = NULL;
    for (char *e = strtok_r(list, ",", &save); e && col < SHOW_PROCS;
         e = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(e, ':');
        if (colon) {
            *colon = '\0';
            printf("   %7s %-20.20s", e, colon + 1);
        } else {
            printf("   %-28.28s", e);
        }
        if (++col % 3 == 0) printf("\n");
    }
    if (col % 3) printf("\n");
    if (count > SHOW_PROCS)
        printf("   ... %d more (full list received; showing first %d)\n",
               count - SHOW_PROCS, SHOW_PROCS);
}

static void print_response(char *line)
{
    size_t len = strlen(line);
    size_t tag = strlen(" " SID_TAG);
    int has_sid = len >= tag && strcmp(line + len - tag, " " SID_TAG) == 0;

    if (strncmp(line, "OK PROCS ", 9) == 0) {
        show_procs(line);
    } else {
        printf("<< %s\n", line);
        if (strncmp(line, "OK SYSINFO ", 11) == 0) show_sysinfo(line);
    }
    if (!has_sid)
        printf("!! warning: response is missing the %s tag\n", SID_TAG);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static ssize_t read_bytes(conn_t *c, char *dst, size_t want)
{
    if (c->buf_len > 0) {
        size_t n = c->buf_len < want ? c->buf_len : want;
        memcpy(dst, c->buf, n);
        memmove(c->buf, c->buf + n, c->buf_len - n);
        c->buf_len -= n;
        return (ssize_t)n;
    }
    for (;;) {
        ssize_t n = recv(c->fd, dst, want, 0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

static void show_rate(const char *what, long long bytes, double secs)
{
    double rate = secs > 0 ? bytes / secs : 0.0;
    printf("   %s %lld bytes in %.3f s = %.0f bytes/s (%.2f MB/s)\n",
           what, bytes, secs, rate, rate / (1024 * 1024));
}

/* returns -1 if the connection is broken */
static int do_put(conn_t *c, const char *local_path, char *reply, size_t reply_size)
{
    int fd = open(local_path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("!! cannot open local file '%s'\n", local_path);
        if (fd >= 0) close(fd);
        return 0;
    }

    char path_copy[512];
    snprintf(path_copy, sizeof path_copy, "%s", local_path);
    const char *name = basename(path_copy);
    long long size = st.st_size;

    char header[600];
    int n = snprintf(header, sizeof header, "PUT %s %lld\n", name, size);
    double start = now_sec();
    if (send_all(c->fd, header, (size_t)n) < 0) { close(fd); return -1; }

    char chunk[CHUNK];
    long long left = size;
    while (left > 0) {
        ssize_t r = read(fd, chunk, left < CHUNK ? (size_t)left : CHUNK);
        if (r <= 0) {
            printf("!! read error on local file, connection must be closed\n");
            close(fd);
            return -1;
        }
        if (send_all(c->fd, chunk, (size_t)r) < 0) { close(fd); return -1; }
        left -= r;
    }
    close(fd);

    if (read_line(c, reply, reply_size) != 1) return -1;
    double secs = now_sec() - start;
    print_response(reply);
    if (strncmp(reply, "OK FILE_RECEIVED", 16) == 0)
        show_rate("uploaded", size, secs);
    return 0;
}

static int do_get(conn_t *c, const char *name, char *reply, size_t reply_size)
{
    char line[600];
    int n = snprintf(line, sizeof line, "GET %s\n", name);
    double start = now_sec();
    if (send_all(c->fd, line, (size_t)n) < 0) return -1;
    if (read_line(c, reply, reply_size) != 1) return -1;

    char rname[256];
    long long size;
    if (sscanf(reply, "OK FILE_SEND %255s %lld", rname, &size) != 2 || size < 0) {
        print_response(reply);
        return 0;
    }
    print_response(reply);

    char name_copy[256], out_path[512];
    snprintf(name_copy, sizeof name_copy, "%s", name);
    mkdir(DOWNLOAD_DIR, 0755);
    snprintf(out_path, sizeof out_path, "%s/%s", DOWNLOAD_DIR, basename(name_copy));

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) printf("!! cannot create %s, data will be discarded\n", out_path);

    char chunk[CHUNK];
    long long left = size;
    while (left > 0) {
        ssize_t r = read_bytes(c, chunk, left < CHUNK ? (size_t)left : CHUNK);
        if (r <= 0) {
            printf("!! connection lost after %lld of %lld bytes\n", size - left, size);
            if (fd >= 0) { close(fd); unlink(out_path); }
            return -1;
        }
        if (fd >= 0 && write(fd, chunk, (size_t)r) != r) {
            printf("!! write error, removing partial file\n");
            close(fd);
            unlink(out_path);
            fd = -1;
        }
        left -= r;
    }
    if (fd >= 0) {
        close(fd);
        printf("   saved to %s\n", out_path);
        show_rate("downloaded", size, now_sec() - start);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : AGENT_PORT;

    signal(SIGPIPE, SIG_IGN);

    conn_t *c = calloc(1, sizeof *c);
    if (!c) return 1;

    c->fd = connect_to_agent(host, port);
    if (c->fd < 0) { free(c); return 1; }

    printf("Connected to RemoteOps Agent at %s:%d\n", host, port);
    printf("Commands: AUTH <token> | SYSINFO | LISTPROC | EXEC <name> |\n"
           "          PUT <local_file> | GET <name> | QUIT\n");

    char input[MAX_INPUT];
    static char reply[BUF_SIZE];

    for (;;) {
        printf("remoteops> ");
        fflush(stdout);

        if (!fgets(input, sizeof input, stdin)) {
            printf("\n");
            strcpy(input, "QUIT");
        }
        input[strcspn(input, "\r\n")] = '\0';
        if (input[0] == '\0') continue;

        if (strncmp(input, "PUT ", 4) == 0 || strncmp(input, "GET ", 4) == 0) {
            char *arg = input + 4;
            while (*arg == ' ') arg++;
            int rc = input[0] == 'P' ? do_put(c, arg, reply, sizeof reply)
                                     : do_get(c, arg, reply, sizeof reply);
            if (rc < 0) { printf("Connection lost.\n"); break; }
            continue;
        }

        char out[MAX_INPUT + 2];
        int n = snprintf(out, sizeof out, "%s\n", input);
        if (send_all(c->fd, out, (size_t)n) < 0) {
            perror("send");
            break;
        }

        int r = read_line(c, reply, sizeof reply);
        if (r == 0) { printf("Agent closed the connection.\n"); break; }
        if (r < 0)  { printf("Connection error.\n"); break; }

        print_response(reply);

        if (strcmp(input, "QUIT") == 0 && strncmp(reply, "OK BYE", 6) == 0)
            break;
    }

    close(c->fd);
    free(c);
    return 0;
}
