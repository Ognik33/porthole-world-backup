// wb.h — общее для всех файлов хелпера world_backup (одна DLL, несколько исходников).
//
// Здесь только то, что нужно сразу нескольким файлам: обёртки платформы (потоки, мьютексы,
// сокеты) и вывод строк в stdout. Реализация вывода — в main.c.
// Обёртки взяты из лабы (lab_probe.c), проверенной на Porthole 3.0.2 / Windows.
#ifndef WB_H
#define WB_H

#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WB_VERSION
#define WB_VERSION "dev"
#endif
#define WB_PROTO 1 // версия протокола агента; Luau сверяет её с hello

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
typedef CRITICAL_SECTION mutex_t;
#define BAD_SOCK INVALID_SOCKET
#define CLOSESOCK closesocket
static inline void mutex_init(mutex_t *m) { InitializeCriticalSection(m); }
static inline void mutex_lock(mutex_t *m) { EnterCriticalSection(m); }
static inline void mutex_unlock(mutex_t *m) { LeaveCriticalSection(m); }
static inline unsigned long long now_ms(void) { return GetTickCount64(); }
typedef DWORD(WINAPI *thread_fn)(void *);
#define THREAD_RET DWORD WINAPI
#define THREAD_RETURN return 0
static inline int thread_start(thread_fn fn, void *arg) {
    HANDLE h = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}
static inline int sock_err(void) { return WSAGetLastError(); }
static inline int sock_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
#else
// Linux-сборка нужна только для тестов протокола; команды Windows отвечают "unsupported"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
typedef pthread_mutex_t mutex_t;
#define BAD_SOCK (-1)
#define CLOSESOCK close
static inline void mutex_init(mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
static inline void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }
static inline unsigned long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
typedef void *(*thread_fn)(void *);
#define THREAD_RET void *
#define THREAD_RETURN return NULL
static inline int thread_start(thread_fn fn, void *arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return 0;
    pthread_detach(t);
    return 1;
}
static inline int sock_err(void) { return errno; }
static inline int sock_would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
#endif

// ---------- вывод (main.c) ----------
// Каждая строка stdout — одно событие для Luau: "<вид>\tключ=значение\t...".
// Porthole отбрасывает поток с невалидным UTF-8, поэтому всё выводимое проходит очистку.

typedef struct { char *p; size_t n, cap; int failed; } sbuf;

void out_line(const char *s);                          // потокобезопасно
int sb_putn(sbuf *b, const char *s, size_t n);         // 0, если не хватило памяти
void sb_put(sbuf *b, const char *s);
void sb_free(sbuf *b);
const char *sb_str(const sbuf *b);
void sb_put_clean(sbuf *b, const char *s, size_t max_bytes); // TAB/CR/LF -> пробел, только валидный UTF-8
void sb_field(sbuf *b, const char *key, const char *val);    // "\tkey=val" с очисткой, до 1024 байт
void sb_field_int(sbuf *b, const char *key, long long v);
void sb_emit(sbuf *b);                                 // вывести и освободить
void emit_error(const char *reason, const char *detail);

#endif
