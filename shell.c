/* server.c
 * Compilar: gcc -O2 -std=c11 -Wall -Wextra -o progman_server server.c
 * Executar: ./progman_server 8080
 *
 * Segurança: aceita e executa comandos só se a conexão vier de localhost.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#define BACKLOG 10
#define BUF_SIZE 8192
#define SMALL 256

/* Structure to hold menu items */
typedef struct {
    char *caption;
    char *command;
} MenuItem;

/* Dynamic array for menu items */
typedef struct {
    MenuItem *items;
    size_t count;
} Menu;

static volatile int keep_running = 1;

void sigint_handler(int s) {
    (void)s;
    keep_running = 0;
}

/* Utility: safe strdup */
char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *r = strdup(s);
    if (!r) { perror("strdup"); exit(1); }
    return r;
}

/* Trim newline and spaces */
void trim_newline(char *s) {
    if (!s) return;
    size_t i = strlen(s);
    while (i && (s[i-1] == '\n' || s[i-1] == '\r')) { s[--i] = '\0'; }
}

/* Read progman.ini */
void load_progman_ini(const char *path, Menu *menu) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Aviso: não foi possível abrir %s: %s\n", path, strerror(errno));
        menu->items = NULL;
        menu->count = 0;
        return;
    }
    char *line = NULL;
    size_t n = 0;
    ssize_t r;
    menu->items = NULL;
    menu->count = 0;
    while ((r = getline(&line, &n, f)) != -1) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        char *cap = line;
        char *cmd = sep + 1;
        if (strlen(cap) == 0 || strlen(cmd) == 0) continue;
        menu->items = realloc(menu->items, sizeof(MenuItem) * (menu->count + 1));
        menu->items[menu->count].caption = xstrdup(cap);
        menu->items[menu->count].command = xstrdup(cmd);
        menu->count++;
    }
    free(line);
    fclose(f);
}

/* Free menu */
void free_menu(Menu *menu) {
    if (!menu) return;
    for (size_t i = 0; i < menu->count; ++i) {
        free(menu->items[i].caption);
        free(menu->items[i].command);
    }
    free(menu->items);
    menu->items = NULL;
    menu->count = 0;
}

/* URL-decode (very small) */
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && isxdigit(a) && isxdigit(b)) {
            char hex[3] = {a, b, 0};
            *dst++ = (char) strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Execute a command and capture output (limited size) */
char *run_command_capture(const char *cmd) {
    FILE *fp;
    size_t cap = 16384;
    size_t used = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    /* Use /bin/sh -c to allow shell commands similar to user's shell */
    char *shcmd;
    if (asprintf(&shcmd, "/bin/sh -c '%s'", cmd) < 0) {
        free(out);
        return NULL;
    }

    fp = popen(shcmd, "r");
    free(shcmd);
    if (!fp) {
        snprintf(out, cap, "Erro ao executar comando: %s\n", strerror(errno));
        return out;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t bl = strlen(buf);
        if (used + bl + 1 > cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) break;
            out = tmp;
        }
        memcpy(out + used, buf, bl);
        used += bl;
        out[used] = '\0';
    }
    int status = pclose(fp);
    /* Optionally append exit code */
    if (WIFEXITED(status)) {
        int ec = WEXITSTATUS(status);
        if (used + 64 > cap) {
            cap += 128;
            out = realloc(out, cap);
        }
        int n = snprintf(out + used, cap - used, "\n(Exit code: %d)\n", ec);
        used += (n>0?n:0);
    }
    return out;
}

/* Build main HTML page from menu */
char *build_main_page(const Menu *menu) {
    const char *tmpl_head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!doctype html>\n"
        "<html><head><meta charset='utf-8'><title>ProgMan</title>\n"
        "<style>\n"
        "body { background: #ffff80; margin:0; font-family: monospace; }\n"
        ".startmenu { position: fixed; left: 10px; top: 10px; background: #eee8b0; border: 2px solid #888; padding: 8px; box-shadow: 4px 4px 6px rgba(0,0,0,0.2); }\n"
        ".menuitem { display:block; margin:4px 0; cursor:pointer; }\n"
        ".content { padding: 20px; }\n"
        ".runform { margin-top:10px; }\n"
        "</style>\n"
        "<script>\n"
        "function sendCommand(cmd) {\n"
        "  var xhr = new XMLHttpRequest();\n"
        "  xhr.open('POST','/run',true);\n        xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');\n"
        "  xhr.onreadystatechange = function() {\n"
        "    if (xhr.readyState==4) {\n"
        "      document.getElementById('out').innerHTML = '<pre>'+escapeHtml(xhr.responseText)+'</pre>';\n" 
        "    }\n"
        "  };\n"
        "  xhr.send('cmd='+encodeURIComponent(cmd));\n"
        "}\n"
        "function escapeHtml(s){ return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }\n"
        "</script>\n"
        "</head><body>\n"
        "<div class='startmenu'><strong>Start</strong>\n";
    /* estimate size */
    size_t cap = strlen(tmpl_head) + 4096;
    for (size_t i = 0; i < menu->count; ++i) cap += strlen(menu->items[i].caption) + strlen(menu->items[i].command) + 128;
    cap += 1024;
    char *resp = malloc(cap);
    if (!resp) return NULL;
    strcpy(resp, tmpl_head);
    for (size_t i = 0; i < menu->count; ++i) {
        char esc_caption[512];
        snprintf(esc_caption, sizeof(esc_caption), "%s", menu->items[i].caption);
        char line[1024];
        snprintf(line, sizeof(line),
                 "<a class='menuitem' onclick=\"sendCommand('%s');\">%s</a>\n",
                 menu->items[i].command, esc_caption);
        strcat(resp, line);
    }
    strcat(resp,
           "<div class='runform'>"
           "<input id='cmd' type='text' size='40' placeholder='Comando manual'/>"
           "<button onclick=\"sendCommand(document.getElementById('cmd').value);\">Run</button>"
           "</div></div>\n"
           "<div class='content'><h2>Painel</h2><div id='out'><em>Resultado aparecerá aqui...</em></div></div>\n"
           "</body></html>\n");
    return resp;
}

/* Simple helper to send 403/400/200 with message */
void send_simple(int fd, const char *status, const char *body) {
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s",
                     status, strlen(body), body);
    send(fd, header, n, 0);
}

/* Check if addr is loopback IPv4 or IPv6 */
int is_local_addr(struct sockaddr_storage *addr) {
    if (!addr) return 0;
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in*)addr;
        uint32_t ip = ntohl(a->sin_addr.s_addr);
        /* 127.0.0.0/8 check */
        return ( (ip >> 24) == 127 );
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6*)addr;
        /* ::1 is all-zero except last byte 1 */
        const unsigned char *b = (const unsigned char*)&a6->sin6_addr;
        /* check ::1 */
        for (int i=0;i<15;i++) if (b[i]!=0) return 0;
        return (b[15] == 1);
    }
    return 0;
}

/* Read exactly n bytes (or until EOF) */
ssize_t read_n(int fd, void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, (char*)buf + off, n - off, 0);
        if (r <= 0) return r;
        off += r;
    }
    return off;
}

/* Very small HTTP header read (reads until \r\n\r\n or buffer full) */
ssize_t read_http_header(int fd, char *buf, size_t cap) {
    size_t off = 0;
    while (off + 1 < cap) {
        ssize_t r = recv(fd, buf + off, 1, 0);
        if (r <= 0) return r;
        off += r;
        if (off >= 4 && strncmp(buf + off - 4, "\r\n\r\n", 4) == 0) {
            buf[off] = '\0';
            return off;
        }
    }
    buf[cap-1] = '\0';
    return off;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    Menu menu = {0};
    load_progman_ini("progman.ini", &menu);

    int listenfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return 1;
    }
    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* Bind to :: (will accept IPv4 mapped too) but we'll check client addr for loopback */
    struct sockaddr_in6 serv6;
    memset(&serv6, 0, sizeof(serv6));
    serv6.sin6_family = AF_INET6;
    serv6.sin6_addr = in6addr_any;
    serv6.sin6_port = htons(port);
    if (bind(listenfd, (struct sockaddr*)&serv6, sizeof(serv6)) < 0) {
        perror("bind");
        close(listenfd);
        return 1;
    }
    if (listen(listenfd, BACKLOG) < 0) {
        perror("listen");
        close(listenfd);
        return 1;
    }
    fprintf(stderr, "Servidor a correr em http://127.0.0.1:%d/  (Ctrl-C para sair)\n", port);

    while (keep_running) {
        struct sockaddr_storage client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int fd = accept(listenfd, (struct sockaddr*)&client_addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) break;
            perror("accept");
            continue;
        }
        /* check local */
        if (!is_local_addr(&client_addr)) {
            send_simple(fd, "403 Forbidden", "Forbidden: only localhost allowed\n");
            close(fd);
            continue;
        }

        /* read header */
        char hdr[BUF_SIZE];
        ssize_t hdrlen = read_http_header(fd, hdr, sizeof(hdr));
        if (hdrlen <= 0) { close(fd); continue; }

        /* parse request-line */
        char method[SMALL], path[SMALL];
        sscanf(hdr, "%s %s", method, path);

        if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
            char *page = build_main_page(&menu);
            if (page) {
                send(fd, page, strlen(page), 0);
                free(page);
            } else {
                send_simple(fd, "500 Internal Server Error", "Erro a construir página\n");
            }
            close(fd);
            continue;
        }

        if (strcmp(method, "POST") == 0 && strcmp(path, "/run") == 0) {
            /* find Content-Length */
            int content_len = 0;
            char *cl = strcasestr(hdr, "Content-Length:");
            if (cl) sscanf(cl, "Content-Length: %d", &content_len);
            if (content_len <= 0 || content_len > 65536) {
                send_simple(fd, "411 Length Required", "Content-Length required or too large\n");
                close(fd);
                continue;
            }
            /* find start of body (hdrlen already) */
            /* we have read up to \r\n\r\n; compute how many bytes of body may already be in hdr buffer */
            char *body = strstr(hdr, "\r\n\r\n");
            int have = 0;
            char *postbuf = malloc(content_len + 1);
            if (!postbuf) { close(fd); continue; }
            if (body) {
                body += 4;
                have = hdrlen - (body - hdr);
                if (have > 0) {
                    int copy = have > content_len ? content_len : have;
                    memcpy(postbuf, body, copy);
                }
            }
            if (have < content_len) {
                ssize_t r = read_n(fd, postbuf + have, content_len - have);
                if (r <= 0) {
                    free(postbuf);
                    close(fd);
                    continue;
                }
            }
            postbuf[content_len] = '\0';
            /* Expect form: cmd=<urlencoded> or similar */
            char *p = strstr(postbuf, "cmd=");
            char cmddec[65536];
            if (!p) {
                send_simple(fd, "400 Bad Request", "No cmd parameter\n");
                free(postbuf);
                close(fd);
                continue;
            } else {
                p += 4;
                url_decode(cmddec, p);
                /* trim possible trailing & or extra */
                char *amp = strchr(cmddec, '&');
                if (amp) *amp = '\0';
            }
            free(postbuf);

            /* Run command and capture output */
            char *out = run_command_capture(cmddec);
            if (!out) out = xstrdup("Erro na execução ou memória insuficiente.\n");

            /* Build an HTML response with yellow background and preformatted output */
            size_t resp_cap = strlen(out) + 1024;
            char *resp = malloc(resp_cap);
            if (!resp) {
                send_simple(fd, "500 Internal Server Error", "Memória insuficiente\n");
                free(out);
                close(fd);
                continue;
            }
            snprintf(resp, resp_cap,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Connection: close\r\n\r\n"
                     "<!doctype html><html><head><meta charset='utf-8'><title>Output</title>\n"
                     "<style>body{background:#ffff80;font-family:monospace;padding:12px;} pre{white-space:pre-wrap;}</style>\n"
                     "</head><body><h3>Comando: %s</h3><pre>%s</pre>\n"
                     "<p><a href='/'>Voltar</a></p></body></html>",
                     cmddec, out);
            send(fd, resp, strlen(resp), 0);
            free(resp);
            free(out);
            close(fd);
            continue;
        }

        /* unknown path */
        send_simple(fd, "404 Not Found", "Not found\n");
        close(fd);
    }

    close(listenfd);
    free_menu(&menu);
    fprintf(stderr, "Servidor terminado\n");
    return 0;
}

