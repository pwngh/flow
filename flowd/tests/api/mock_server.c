/* tests/api/mock_server.c
 *
 * Thread-per-request HTTP server for adapter tests. See
 * mock_server.h for the contract.
 *
 * Implementation:
 *   - Bind port 0 (kernel picks free port), capture via getsockname.
 *   - listen(1), spawn a worker thread that accept()s one client.
 *   - Worker reads until end of headers, parses Content-Length,
 *     reads the body, writes the canned response, closes the fd.
 *   - Captured request bytes are stored on the server struct for
 *     post-test inspection.
 *
 * Errors during the worker are silent — tests fail at the
 * assertion stage, not in the server. The server's job is to
 * deliver determinism, not to police the test.
 */

#include "mock_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* POSIX-strict mode drops strcasestr and INADDR_LOOPBACK; inline
 * what we need. */
static const char *
ci_strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen
               && tolower((unsigned char)p[i])
                  == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen) return p;
    }
    return NULL;
}


struct mock_server {
    int             listen_fd;
    uint16_t        port;
    pthread_t       thread;
    char           *response;       /* heap-owned */
    mock_capture_t  capture;        /* heap-owned fields */
    int             ready_pipe[2];  /* signals "thread is listening" */
};


static char *
slurp_until_end_of_headers(int fd, size_t *out_len)
{
    size_t cap = 4096, len = 0;
    char  *buf = malloc(cap);
    for (;;) {
        if (len + 1u >= cap) {
            cap *= 2u;
            char *g = realloc(buf, cap);
            if (!g) { free(buf); return NULL; }
            buf = g;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1u);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) break;
        if (strstr(buf, "\n\n")     != NULL) break;
    }
    *out_len = len;
    return buf;
}


static void *
worker(void *user)
{
    mock_server_t *m = user;

    /* Signal readiness. */
    char r = '1';
    write(m->ready_pipe[1], &r, 1);

    int client = accept(m->listen_fd, NULL, NULL);
    if (client < 0) return NULL;

    /* Read until the end of headers. */
    size_t hlen = 0;
    char *headers = slurp_until_end_of_headers(client, &hlen);
    if (!headers) { close(client); return NULL; }

    /* Find Content-Length and the body offset. */
    size_t content_length = 0;
    const char *cl = ci_strstr(headers, "content-length:");
    if (cl) content_length = (size_t)strtoul(cl + 15, NULL, 10);
    const char *body_start = strstr(headers, "\r\n\r\n");
    size_t body_in_headers = 0;
    if (body_start) {
        body_start += 4;
        body_in_headers = hlen - (size_t)(body_start - headers);
    } else {
        body_start = strstr(headers, "\n\n");
        if (body_start) {
            body_start += 2;
            body_in_headers = hlen - (size_t)(body_start - headers);
        }
    }

    /* Read the rest of the body. */
    char *body = malloc(content_length + 1u);
    size_t body_have = 0;
    if (body_start && body_in_headers > 0) {
        size_t take = body_in_headers < content_length
                    ? body_in_headers : content_length;
        memcpy(body, body_start, take);
        body_have = take;
    }
    while (body_have < content_length) {
        ssize_t n = read(client, body + body_have,
                         content_length - body_have);
        if (n <= 0) break;
        body_have += (size_t)n;
    }
    body[body_have] = '\0';

    /* Save the capture. */
    m->capture.request     = headers;
    m->capture.request_len = hlen;
    m->capture.body        = body;
    m->capture.body_len    = body_have;

    /* Send the canned response. */
    size_t rlen = strlen(m->response);
    size_t sent = 0;
    while (sent < rlen) {
        ssize_t n = write(client, m->response + sent, rlen - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(client);
    return NULL;
}


mock_server_t *
mock_server_start(const char *response)
{
    mock_server_t *m = calloc(1, sizeof *m);
    m->response = strdup(response);

    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd < 0) { free(m->response); free(m); return NULL; }

    int yes = 1;
    setsockopt(m->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = 0;
    if (bind(m->listen_fd, (struct sockaddr *)&addr,
             sizeof addr) < 0) {
        close(m->listen_fd); free(m->response); free(m); return NULL;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(m->listen_fd, (struct sockaddr *)&addr,
                    &alen) < 0) {
        close(m->listen_fd); free(m->response); free(m); return NULL;
    }
    m->port = ntohs(addr.sin_port);

    if (listen(m->listen_fd, 1) < 0) {
        close(m->listen_fd); free(m->response); free(m); return NULL;
    }

    pipe(m->ready_pipe);
    pthread_create(&m->thread, NULL, worker, m);
    /* Wait until the worker actually called accept() (the ready
     * byte is written immediately before). */
    char r;
    read(m->ready_pipe[0], &r, 1);
    return m;
}

uint16_t
mock_server_port(const mock_server_t *m)
{
    return m ? m->port : 0;
}

const mock_capture_t *
mock_server_captured(mock_server_t *m)
{
    return m ? &m->capture : NULL;
}

void
mock_server_stop(mock_server_t *m)
{
    if (!m) return;
    pthread_join(m->thread, NULL);
    close(m->listen_fd);
    close(m->ready_pipe[0]);
    close(m->ready_pipe[1]);
    free(m->response);
    free(m->capture.request);
    free(m->capture.body);
    free(m);
}
