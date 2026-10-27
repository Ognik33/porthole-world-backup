// world_backup — нативный хелпер дополнения «World backup» для Porthole.
//
// Контракт Porthole (как у game_probe и lab_probe): int porthole_native_main(int argc, char **argv).
// argv содержит только наши аргументы. Вывод — строки UTF-8 в stdout, поля через TAB, "ключ=значение".
//
// Режимы:
//   --agent <port> <token> [idle_s]
//                            основной: один процесс на сессию, команды по TCP на 127.0.0.1:<port>.
//                            Если idle_s секунд (по умолчанию 60) не пришло ни одной команды с верным
//                            токеном, агент завершается сам: Luau шлёт ping раз в 10 с, так что тишина
//                            значит, что дополнение больше не работает, и хелпер не должен остаться
//                            сиротой (в логе 0.0.3 после выхода из лобби дополнение не получило ни
//                            одного события, и неизвестно, убил ли Porthole хелпер).
//   --hello                  одна строка hello и выход (диагностика)
//
// Протокол агента (версия 1) описан в docs/protocol.md. Коротко:
//   команда:  <token> TAB <команда> [TAB аргумент]... LF
//   события:  hello proto=1 version=.. caps=..  |  ready port=..  |  pong  |  error reason=.. [detail=..]
//
// Чем агент отличается от лабы. В лабе соединение читалось до закрытия (или 2 с тишины), и только
// потом выполнялись команды: это годилось для porthole.tcp_send (одно соединение на команду), но не
// для porthole.tcp_open (одно постоянное соединение). Здесь агент держит до MAX_CONN соединений
// одновременно и выполняет каждую строку сразу, как только она пришла. Поэтому один и тот же агент
// работает с обоими способами отправки. Ответы идут только в stdout, в сокеты агент ничего не пишет.
#include "wb.h"

#define MAX_CONN 16          // одновременных соединений с агентом
#define MAX_LINE 65536       // длиннее строка не бывает; иначе соединение закрывается
#define MAX_ARGS 8

// ======================================================================
// вывод
// ======================================================================

static mutex_t g_out_lock;
static volatile int g_out_ok = 1; // 0, когда stdout закрыт: Porthole нас больше не слушает
static volatile int g_quit = 0;
static unsigned long long g_last_cmd = 0; // время последней команды с верным токеном (now_ms)

void out_line(const char *s) {
    mutex_lock(&g_out_lock);
    if (g_out_ok) {
        size_t n = strlen(s);
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD w;
        if (!h || h == INVALID_HANDLE_VALUE || !WriteFile(h, s, (DWORD)n, &w, NULL) || !WriteFile(h, "\n", 1, &w, NULL))
            g_out_ok = 0;
#else
        if (fwrite(s, 1, n, stdout) != n || fputc('\n', stdout) == EOF || fflush(stdout) != 0) g_out_ok = 0;
#endif
    }
    mutex_unlock(&g_out_lock);
}

int sb_putn(sbuf *b, const char *s, size_t n) {
    if (b->failed) return 0;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { b->failed = 1; return 0; }
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}
void sb_put(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
void sb_free(sbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; b->failed = 0; }
const char *sb_str(const sbuf *b) { return b->p ? b->p : ""; }

void sb_put_clean(sbuf *b, const char *s, size_t max_bytes) {
    const unsigned char *u = (const unsigned char *)s;
    size_t start = b->n;
    while (*u && b->n - start < max_bytes) {
        unsigned char c = *u;
        if (c == '\t' || c == '\r' || c == '\n') { sb_putn(b, " ", 1); u++; continue; }
        if (c < 0x20 || c == 0x7F) { u++; continue; }
        if (c < 0x80) { sb_putn(b, (const char *)u, 1); u++; continue; }
        int len = (c >= 0xF0 && c <= 0xF4) ? 4 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xC2 && c <= 0xDF) ? 2 : 0;
        int ok = len > 0;
        for (int i = 1; ok && i < len; i++) if ((u[i] & 0xC0) != 0x80) ok = 0;
        if (ok && b->n - start + (size_t)len > max_bytes) break; // не резать символ пополам
        if (ok) { sb_putn(b, (const char *)u, (size_t)len); u += len; }
        else { sb_putn(b, "?", 1); u++; }
    }
}

void sb_field(sbuf *b, const char *key, const char *val) {
    sb_put(b, "\t");
    sb_put(b, key);
    sb_put(b, "=");
    sb_put_clean(b, val ? val : "", 1024);
}

void sb_field_int(sbuf *b, const char *key, long long v) {
    char n[32];
    snprintf(n, sizeof n, "%lld", v);
    sb_field(b, key, n);
}

void sb_emit(sbuf *b) {
    if (b->failed) out_line("error\treason=memory");
    else out_line(sb_str(b));
    sb_free(b);
}

void emit_error(const char *reason, const char *detail) {
    sbuf l = {0};
    sb_put(&l, "error");
    sb_field(&l, "reason", reason);
    if (detail) sb_field(&l, "detail", detail);
    sb_emit(&l);
}

static void emit_hello(void) {
    sbuf l = {0};
    sb_put(&l, "hello");
    sb_field_int(&l, "proto", WB_PROTO);
    sb_field(&l, "version", WB_VERSION);
    // persist — агент понимает постоянные соединения (porthole.tcp_open)
    sb_field(&l, "caps", "persist");
    sb_emit(&l);
}

// ======================================================================
// сокеты
// ======================================================================

static int net_init(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
        done = 1;
    }
#endif
    return 1;
}

static int set_nonblocking(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

// ======================================================================
// команды
// ======================================================================

static char g_token[65];

// сравнение за постоянное время: по времени ответа нельзя подобрать токен
static int token_ok(const char *got) {
    size_t na = strlen(got), nb = strlen(g_token);
    unsigned char d = (unsigned char)(na != nb);
    for (size_t i = 0; i < na && i < nb; i++) d |= (unsigned char)(got[i] ^ g_token[i]);
    return d == 0 && nb > 0;
}

// line изменяется на месте (TAB -> 0). Возвращает 0, если соединение надо закрыть (чужой токен).
static int handle_command(char *line) {
    char *parts[MAX_ARGS + 2];
    int n = 0;
    char *p = line;
    while (n < MAX_ARGS + 2) {
        parts[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    if (n < 2 || !token_ok(parts[0])) {
        emit_error("bad_token", NULL);
        return 0;
    }
    g_last_cmd = now_ms();
    const char *cmd = parts[1];
    if (!strcmp(cmd, "ping")) out_line("pong");
    else if (!strcmp(cmd, "quit")) g_quit = 1;
    // шаг 2: resolve (resolve.c); шаг 3: backup, list, prune, space, pickfolder, open (backup.c)
    else emit_error("bad_command", cmd);
    return 1;
}

// ======================================================================
// агент
// ======================================================================

typedef struct {
    sock_t s;
    sbuf in;                  // принятые, но ещё не выполненные байты
    unsigned long long last;  // время последней активности, для вытеснения при переполнении
} conn_t;

static conn_t g_conn[MAX_CONN];

static void conn_close(conn_t *c) {
    if (c->s != BAD_SOCK) CLOSESOCK(c->s);
    c->s = BAD_SOCK;
    sb_free(&c->in);
}

// выполнить все целые строки из буфера; at_eof — выполнить и хвост без LF.
// Возвращает 0, если соединение нужно закрыть.
static int conn_process(conn_t *c, int at_eof) {
    size_t off = 0;
    int keep = 1;
    while (keep && off < c->in.n) {
        char *start = c->in.p + off;
        char *nl = memchr(start, '\n', c->in.n - off);
        if (!nl && !at_eof) break;
        size_t len = nl ? (size_t)(nl - start) : c->in.n - off;
        start[len] = 0;
        if (len && start[len - 1] == '\r') start[len - 1] = 0;
        if (*start) keep = handle_command(start);
        off += len + (nl ? 1 : 0);
    }
    if (!keep) return 0;
    if (off > 0) {
        memmove(c->in.p, c->in.p + off, c->in.n - off);
        c->in.n -= off;
        if (c->in.p) c->in.p[c->in.n] = 0;
    }
    if (c->in.n > MAX_LINE) {
        emit_error("line_too_long", NULL);
        return 0;
    }
    return 1;
}

static void conn_add(sock_t s) {
    conn_t *slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_CONN; i++) {
        if (g_conn[i].s == BAD_SOCK) { slot = &g_conn[i]; break; }
        if (!oldest || g_conn[i].last < oldest->last) oldest = &g_conn[i];
    }
    if (!slot) {
        // все места заняты: новое соединение важнее самого давно молчащего
        conn_close(oldest);
        slot = oldest;
    }
    set_nonblocking(s);
    slot->s = s;
    slot->in = (sbuf){0};
    slot->last = now_ms();
}

static void conn_read(conn_t *c) {
    char buf[4096];
    int got = recv(c->s, buf, sizeof buf, 0);
    if (got > 0) {
        c->last = now_ms();
        if (!sb_putn(&c->in, buf, (size_t)got)) { emit_error("memory", NULL); conn_close(c); return; }
        if (!conn_process(c, 0)) conn_close(c);
    } else if (got == 0) {
        conn_process(c, 1); // tcp_send: строка могла прийти без LF прямо перед закрытием
        conn_close(c);
    } else if (!sock_would_block(sock_err())) {
        conn_close(c);
    }
}

static int mode_agent(const char *port_s, const char *token, const char *idle_s) {
    int port = port_s ? atoi(port_s) : 0;
    int idle = idle_s ? atoi(idle_s) : 60;
    if (port < 1 || port > 65535 || !token || strlen(token) < 16 || strlen(token) > 64 || idle < 1 || idle > 86400) {
        emit_error("bad_args", NULL);
        return 2;
    }
    snprintf(g_token, sizeof g_token, "%s", token);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // вторая сторона закрыла соединение — не повод падать
#endif
    if (!net_init()) { emit_error("socket", NULL); return 3; }
    for (int i = 0; i < MAX_CONN; i++) g_conn[i].s = BAD_SOCK;

    sock_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == BAD_SOCK) { emit_error("socket", NULL); return 3; }
#ifdef _WIN32
    { BOOL ex = TRUE; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&ex, sizeof ex); }
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7F000001); // только loopback: из сети агент недоступен
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0 || !set_nonblocking(ls)) {
        CLOSESOCK(ls);
        emit_error("bind", NULL); // Luau перезапустит агент на другом порту
        return 3;
    }
    emit_hello();
    g_last_cmd = now_ms();
    {
        char r[32];
        snprintf(r, sizeof r, "ready\tport=%d", port);
        out_line(r);
    }

    while (!g_quit && g_out_ok) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        sock_t maxfd = ls;
        for (int i = 0; i < MAX_CONN; i++) {
            if (g_conn[i].s == BAD_SOCK) continue;
            FD_SET(g_conn[i].s, &rd);
            if (g_conn[i].s > maxfd) maxfd = g_conn[i].s;
        }
        struct timeval tv = { 1, 0 };
        int r = select((int)maxfd + 1, &rd, NULL, NULL, &tv);
        if (now_ms() - g_last_cmd >= (unsigned long long)idle * 1000ull) {
            emit_error("idle_timeout", NULL);
            break;
        }
        if (r <= 0) continue; // таймаут: проверить g_quit / g_out_ok
        if (FD_ISSET(ls, &rd)) {
            for (;;) {
                sock_t c = accept(ls, NULL, NULL);
                if (c == BAD_SOCK) break;
                conn_add(c);
            }
        }
        for (int i = 0; i < MAX_CONN; i++) {
            if (g_conn[i].s != BAD_SOCK && FD_ISSET(g_conn[i].s, &rd)) conn_read(&g_conn[i]);
        }
    }
    for (int i = 0; i < MAX_CONN; i++) conn_close(&g_conn[i]);
    CLOSESOCK(ls);
    return 0;
}

// ======================================================================
// точка входа
// ======================================================================

#ifdef _WIN32
__declspec(dllexport)
#endif
int porthole_native_main(int argc, char **argv) {
    mutex_init(&g_out_lock);
    int i = 0;
    if (argc > 0 && argv[0] && argv[0][0] != '-') i = 1; // на случай, если пришло имя программы
    if (i < argc && !strcmp(argv[i], "--agent"))
        return mode_agent(i + 1 < argc ? argv[i + 1] : NULL, i + 2 < argc ? argv[i + 2] : NULL,
                          i + 3 < argc ? argv[i + 3] : NULL);
    if (i < argc && !strcmp(argv[i], "--hello")) { emit_hello(); return 0; }
    emit_error("bad_args", NULL);
    return 2;
}

#if defined(TEST_MAIN) || defined(CLI_MAIN)
int main(int argc, char **argv) { return porthole_native_main(argc - 1, argv + 1); }
#endif
