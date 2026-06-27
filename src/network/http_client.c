/*
 * http_client.c — HTTPS client using raw OpenSSL sockets
 *
 * Zero external dependencies beyond libssl + libcrypto.
 * Implements HTTP/1.1 request/response over TLS.
 * Supports mock mode for testing (via http_mock.h).
 */
#include "cgent.h"
#include "network.h"
#include "platform.h"
#include "http_mock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* ── URL parsing ────────────────────────────────────────────────── */

parsed_url_t *url_parse(const char *url) {
    parsed_url_t *pu = calloc(1, sizeof(parsed_url_t));
    if (!pu) return NULL;

    const char *p = url;

    /* Scheme */
    if (strncmp(p, "https://", 8) == 0) {
        pu->scheme = strdup("https");
        pu->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        pu->scheme = strdup("http");
        pu->port = 80;
        p += 7;
    } else {
        /* Assume https */
        pu->scheme = strdup("https");
        pu->port = 443;
    }

    /* Host */
    const char *host_start = p;
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/')
        host_end++;
    size_t host_len = host_end - host_start;
    if (host_len > 0) {
        pu->host = malloc(host_len + 1);
        memcpy(pu->host, host_start, host_len);
        pu->host[host_len] = '\0';
    }
    p = host_end;

    /* Port */
    if (*p == ':') {
        p++;
        pu->port = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    }

    /* Path */
    if (*p == '\0') {
        pu->path = strdup("/");
    } else {
        pu->path = strdup(p);
    }

    return pu;
}

void url_free(parsed_url_t *url) {
    if (!url) return;
    free(url->scheme);
    free(url->host);
    free(url->path);
    free(url);
}

/* ── TLS connection ─────────────────────────────────────────────── */

static SSL_CTX *ssl_ctx = NULL;

int http_init(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        fprintf(stderr, "SSL_CTX_new failed\n");
        return -1;
    }
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    return 0;
}

void http_cleanup(void) {
    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
        ssl_ctx = NULL;
    }
}

typedef struct {
    int sockfd;
    SSL *ssl;
} tls_conn_t;

static tls_conn_t *tls_connect(const char *host, int port) {
    /* Resolve hostname */
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo *result;
    int rc = getaddrinfo(host, port_str, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return NULL;
    }

    /* Connect to first resolved address */
    int sockfd = -1;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(result);

    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host, port);
        return NULL;
    }

    /* TLS handshake */
    SSL *ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        close(sockfd);
        return NULL;
    }
    SSL_set_fd(ssl, sockfd);
    SSL_set_tlsext_host_name(ssl, host);

    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "SSL_connect failed: %s\n",
                ERR_error_string(ERR_get_error(), NULL));
        SSL_free(ssl);
        close(sockfd);
        return NULL;
    }

    tls_conn_t *conn = malloc(sizeof(tls_conn_t));
    conn->sockfd = sockfd;
    conn->ssl = ssl;
    return conn;
}

static void tls_close(tls_conn_t *conn) {
    if (!conn) return;
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->sockfd >= 0) close(conn->sockfd);
    free(conn);
}

static int tls_send_all(tls_conn_t *conn, const char *data, size_t len) {
    while (len > 0) {
        int n = SSL_write(conn->ssl, data, len);
        if (n <= 0) return -1;
        data += n;
        len -= n;
    }
    return 0;
}

/* Read a line from TLS connection (up to \r\n or \n) */
static char *tls_read_line(tls_conn_t *conn) {
    size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

    while (1) {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        int n = SSL_read(conn->ssl, &buf[len], 1);
        if (n <= 0) { free(buf); return NULL; }
        if (buf[len] == '\r') continue;
        if (buf[len] == '\n') { buf[len] = '\0'; return buf; }
        len++;
    }
}

/* Read exactly n bytes */
static char *tls_read_n(tls_conn_t *conn, size_t n) {
    char *buf = malloc(n + 1);
    if (!buf) return NULL;
    size_t total = 0;
    while (total < n) {
        int r = SSL_read(conn->ssl, buf + total, n - total);
        if (r <= 0) { free(buf); return NULL; }
        total += r;
    }
    buf[n] = '\0';
    return buf;
}

/* ── HTTP request/response ──────────────────────────────────────── */

static char *build_http_request(const http_request_t *req, size_t *out_len) {
    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int len = 0;

    /* Request line */
    parsed_url_t *url = url_parse(req->url);
    if (!url) { free(buf); return NULL; }

    len = snprintf(buf, cap, "%s %s HTTP/1.1\r\n", req->method, url->path);
    /* Host header */
    len += snprintf(buf + len, cap - len, "Host: %s\r\n", url->host);
    /* Default headers */
    len += snprintf(buf + len, cap - len,
                     "User-Agent: cgent/" CGENT_VERSION "\r\n");
    len += snprintf(buf + len, cap - len, "Accept: application/json\r\n");
    len += snprintf(buf + len, cap - len, "Connection: close\r\n");

    /* Custom headers */
    for (int i = 0; i < req->header_count; i++) {
        len += snprintf(buf + len, cap - len, "%s\r\n", req->headers[i]);
    }

    /* Body */
    if (req->body && req->body_length > 0) {
        len += snprintf(buf + len, cap - len,
                         "Content-Type: application/json\r\n");
        len += snprintf(buf + len, cap - len,
                         "Content-Length: %zu\r\n", req->body_length);
    }

    /* End headers */
    len += snprintf(buf + len, cap - len, "\r\n");

    /* Append body */
    if (req->body && req->body_length > 0) {
        if (len + req->body_length + 1 > cap) {
            cap = len + req->body_length + 1;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, req->body, req->body_length);
        len += req->body_length;
    }

    url_free(url);
    *out_len = len;
    buf[len] = '\0';
    return buf;
}

http_response_t *http_request(const http_request_t *req) {
    if (!req || !req->url) return NULL;

    /* Route through mock backend when enabled */
    if (http_mock_is_enabled()) {
        return http_mock_request(req);
    }

    if (!ssl_ctx && http_init() != 0) return NULL;

    parsed_url_t *url = url_parse(req->url);
    if (!url) return NULL;

    bool is_https = (strcmp(url->scheme, "https") == 0);

    http_response_t *resp = calloc(1, sizeof(http_response_t));
    if (!resp) { url_free(url); return NULL; }

    if (is_https) {
        tls_conn_t *conn = tls_connect(url->host, url->port);
        if (!conn) { url_free(url); free(resp); return NULL; }

        /* Build and send request */
        size_t req_len;
        char *req_buf = build_http_request(req, &req_len);
        if (!req_buf || tls_send_all(conn, req_buf, req_len) != 0) {
            free(req_buf); tls_close(conn); url_free(url); free(resp);
            return NULL;
        }
        free(req_buf);

        /* Read status line */
        char *status_line = tls_read_line(conn);
        if (!status_line) { tls_close(conn); url_free(url); free(resp); return NULL; }

        /* Parse status code */
        char *sp = strchr(status_line, ' ');
        resp->status_code = sp ? atoi(sp + 1) : 0;
        free(status_line);

        /* Read headers */
        int hdr_cap = 16;
        resp->headers = malloc(hdr_cap * sizeof(char *));
        resp->header_count = 0;

        while (1) {
            char *hdr = tls_read_line(conn);
            if (!hdr || *hdr == '\0') { free(hdr); break; }

            if (resp->header_count >= hdr_cap) {
                hdr_cap *= 2;
                resp->headers = realloc(resp->headers, hdr_cap * sizeof(char *));
            }
            resp->headers[resp->header_count++] = hdr;
        }

        /* Check for Content-Length or Transfer-Encoding: chunked */
        bool chunked = false;
        size_t content_length = 0;
        for (int i = 0; i < resp->header_count; i++) {
            if (strncasecmp(resp->headers[i], "transfer-encoding:", 18) == 0) {
                if (strstr(resp->headers[i] + 18, "chunked"))
                    chunked = true;
            }
            if (strncasecmp(resp->headers[i], "content-length:", 15) == 0) {
                content_length = atol(resp->headers[i] + 15);
            }
        }

        /* Read body */
        if (chunked) {
            size_t buf_cap = 65536;
            char *body = malloc(buf_cap);
            size_t body_len = 0;

            while (1) {
                char *chunk_line = tls_read_line(conn);
                if (!chunk_line) break;
                /* Parse chunk size (hex) */
                long chunk_size = strtol(chunk_line, NULL, 16);
                free(chunk_line);
                if (chunk_size == 0) break; /* Last chunk */

                if (body_len + chunk_size + 1 > buf_cap) {
                    buf_cap = body_len + chunk_size + 65536;
                    body = realloc(body, buf_cap);
                }
                char *chunk_data = tls_read_n(conn, chunk_size);
                if (chunk_data) {
                    memcpy(body + body_len, chunk_data, chunk_size);
                    body_len += chunk_size;
                    free(chunk_data);
                }
                /* Discard chunk trailing \r\n */
                tls_read_line(conn);
            }
            body[body_len] = '\0';
            resp->body = body;
            resp->body_length = body_len;

            /* Discard trailer headers */
            while (1) {
                char *trailer = tls_read_line(conn);
                if (!trailer || *trailer == '\0') { free(trailer); break; }
                free(trailer);
            }
        } else if (content_length > 0) {
            resp->body = tls_read_n(conn, content_length);
            resp->body_length = content_length;
        } else {
            /* Read until connection close */
            size_t buf_cap = 65536;
            char *body = malloc(buf_cap);
            size_t body_len = 0;

            while (1) {
                if (body_len + 4096 >= buf_cap) {
                    buf_cap *= 2;
                    body = realloc(body, buf_cap);
                }
                int n = SSL_read(conn->ssl, body + body_len, buf_cap - body_len - 1);
                if (n <= 0) break;
                body_len += n;
            }
            body[body_len] = '\0';
            resp->body = body;
            resp->body_length = body_len;
        }

        tls_close(conn);
    }
    /* else: HTTP (non-TLS) — not implemented yet */

    url_free(url);
    return resp;
}

/* ── Response lifecycle ─────────────────────────────────────────── */

void http_response_free(http_response_t *resp) {
    if (!resp) return;
    for (int i = 0; i < resp->header_count; i++) free(resp->headers[i]);
    free(resp->headers);
    free(resp->body);
    free(resp);
}

void http_request_free(http_request_t *req) {
    if (!req) return;
    free(req->method);
    free(req->url);
    for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
    free(req->headers);
    free(req->body);
    free(req);
}
