// backup.c — копии миров (шаг 3 плана, первый заход): zip, backup, list, prune, space, paths.
//
// Где лежат копии: <папка копий>\<имя папки мира>\<ГГГГ-ММ-ДД_ЧЧ-ММ-СС>_<clean|hot>.zip
//   В папке мира лежит метка .world-backup с полным путём мира. Если папка с таким именем уже
//   занята другим миром (одинаковые имена из разных инстансов) или чужая (без метки), используется
//   «<имя> (<хеш пути>)».
//   Время изменения архива = момент начала копии: по нему видно, менялся ли мир после неё.
//
// Архив: папка мира как есть, deflate, как у ванильной кнопки «Создать резервную копию».
//   Пишется в <имя>.zip.tmp и переименовывается только целиком готовым. ZIP64 включается сам,
//   когда файл, архив или число записей не влезают в обычный zip; имена в UTF-8 (флаг 11).
//   session.lock в архив не попадает: он занят игрой и при восстановлении не нужен.
//
// Файловый слой ниже работает и на Linux: так архивы проверяются тестами (tests/backup_test.py)
// без Windows. Пути везде UTF-8, на Windows внутри — UTF-16 с префиксом \\?\ (длинные пути).
#include "wb.h"
#include "third_party/miniz/miniz.h"
#include <time.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#endif

#define CHUNK (1 << 20)            // байт за одно чтение
#define DEFLATE_LEVEL 3            // для обычных файлов: быстрее уровня 6 почти без потери
// Свободного места нужно: размер мира + 10 % + 256 МБ. Архив не больше мира (данные мира почти
// не сжимаются, deflate добавляет доли процента), .tmp переименовывается без копирования; запас
// оставляем, чтобы копия не забила диск до нуля.
#define SPACE_NEED(total) ((total) + (total) / 10 + 256ULL * 1024 * 1024)
#define MARKER ".world-backup"
#define Z64_FILE 0xF0000000ULL     // файл от этого размера пишется сразу с полями ZIP64

#ifdef _WIN32
#define SEP '\\'
#else
#define SEP '/'
#endif

// ======================================================================
// файловый слой
// ======================================================================

#ifdef _WIN32
#include <shlobj.h>
typedef HANDLE fh_t;
#define BAD_FH INVALID_HANDLE_VALUE

// "C:\x" -> "\\?\C:\x", "\\srv\share\x" -> "\\?\UNC\srv\share\x" (malloc)
static wchar_t *wp(const char *u) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n + 8));
    if (!w) return NULL;
    int unc = u[0] == '\\' && u[1] == '\\';
    wcscpy(w, unc ? L"\\\\?\\UNC" : L"\\\\?\\");
    size_t pre = wcslen(w);
    MultiByteToWideChar(CP_UTF8, 0, unc ? u + 1 : u, -1, w + pre, n);
    return w;
}

static long long ft_to_unix(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static fh_t f_open_read(const char *p) {
    wchar_t *w = wp(p);
    if (!w) return BAD_FH;
    // полный совместный доступ: игра может держать файл открытым (горячая копия)
    fh_t h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(w);
    return h;
}
static fh_t f_create(const char *p) {
    wchar_t *w = wp(p);
    if (!w) return BAD_FH;
    fh_t h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    return h;
}
static long long f_read(fh_t h, void *buf, size_t n) {
    DWORD got = 0;
    return ReadFile(h, buf, (DWORD)n, &got, NULL) ? (long long)got : -1;
}
static int f_write(fh_t h, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        DWORD w = 0, part = n > (1u << 30) ? (1u << 30) : (DWORD)n;
        if (!WriteFile(h, p, part, &w, NULL) || w == 0) return 0;
        p += w;
        n -= w;
    }
    return 1;
}
static int f_seek(fh_t h, unsigned long long off) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) != 0;
}
static int f_flush(fh_t h) { return FlushFileBuffers(h) != 0; }
static int f_truncate(fh_t h, unsigned long long len) { return f_seek(h, len) && SetEndOfFile(h); }
static void f_close(fh_t h) { CloseHandle(h); }
static int f_set_mtime(fh_t h, long long t) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)t * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return SetFileTime(h, NULL, NULL, &ft) != 0;
}
static int f_rename(const char *a, const char *b) {
    wchar_t *wa = wp(a), *wb = wp(b);
    int ok = wa && wb && MoveFileExW(wa, wb, MOVEFILE_WRITE_THROUGH);
    free(wa);
    free(wb);
    return ok;
}
static int f_delete(const char *p) {
    wchar_t *w = wp(p);
    int ok = w && DeleteFileW(w);
    free(w);
    return ok;
}
// 1 — каталог, 0 — файл, -1 — нет
static int f_stat(const char *p, unsigned long long *size, long long *mtime) {
    wchar_t *w = wp(p);
    WIN32_FILE_ATTRIBUTE_DATA d;
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok) return -1;
    if (size) *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (mtime) *mtime = ft_to_unix(&d.ftLastWriteTime);
    return (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}
static int f_mkdir(const char *p) {
    wchar_t *w = wp(p);
    int ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(w);
    return ok;
}
// обход одного каталога: cb(ctx, имя, каталог?, размер, mtime); 0 из cb — остановиться
typedef int (*dir_cb)(void *ctx, const char *name, int isdir, unsigned long long size, long long mtime);
static int f_listdir(const char *p, dir_cb cb, void *ctx) {
    size_t n = strlen(p);
    char *pat = malloc(n + 3);
    if (!pat) return 0;
    snprintf(pat, n + 3, "%s\\*", p);
    wchar_t *w = wp(pat);
    free(pat);
    if (!w) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(w);
    if (fh == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    int go = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        // ссылки и точки соединения не обходим: внутри мира их не бывает, а петлю они дать могут
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isdir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL) <= 0) continue;
        unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        go = cb(ctx, name, isdir, size, ft_to_unix(&fd.ftLastWriteTime));
    } while (go && FindNextFileW(fh, &fd));
    FindClose(fh);
    return 1;
}
static int f_free(const char *p, unsigned long long *free_b, unsigned long long *total) {
    wchar_t *w = wp(p);
    ULARGE_INTEGER a, t, f;
    int ok = w && GetDiskFreeSpaceExW(w, &a, &t, &f);
    free(w);
    if (ok) { *free_b = a.QuadPart; *total = t.QuadPart; }
    return ok;
}
static void local_tm(time_t t, struct tm *out) { localtime_s(out, &t); }
#else
typedef int fh_t;
#define BAD_FH (-1)
static fh_t f_open_read(const char *p) { return open(p, O_RDONLY); }
static fh_t f_create(const char *p) { return open(p, O_RDWR | O_CREAT | O_TRUNC, 0644); }
static long long f_read(fh_t h, void *buf, size_t n) { return (long long)read(h, buf, n); }
static int f_write(fh_t h, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(h, p, n);
        if (w <= 0) return 0;
        p += w;
        n -= (size_t)w;
    }
    return 1;
}
static int f_seek(fh_t h, unsigned long long off) { return lseek(h, (off_t)off, SEEK_SET) == (off_t)off; }
static int f_flush(fh_t h) { return fsync(h) == 0; }
static int f_truncate(fh_t h, unsigned long long len) { return ftruncate(h, (off_t)len) == 0; }
static void f_close(fh_t h) { close(h); }
static int f_set_mtime(fh_t h, long long t) {
    struct timeval tv[2] = { { (time_t)t, 0 }, { (time_t)t, 0 } };
    return futimes(h, tv) == 0;
}
static int f_rename(const char *a, const char *b) { return rename(a, b) == 0; }
static int f_delete(const char *p) { return unlink(p) == 0; }
static int f_stat(const char *p, unsigned long long *size, long long *mtime) {
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    if (size) *size = (unsigned long long)st.st_size;
    if (mtime) *mtime = (long long)st.st_mtime;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}
static int f_mkdir(const char *p) { return mkdir(p, 0755) == 0 || errno == EEXIST; }
typedef int (*dir_cb)(void *ctx, const char *name, int isdir, unsigned long long size, long long mtime);
static int f_listdir(const char *p, dir_cb cb, void *ctx) {
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    int go = 1;
    char full[RS_PATH * 2];
    while (go && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", p, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;
        go = cb(ctx, e->d_name, S_ISDIR(st.st_mode), (unsigned long long)st.st_size, (long long)st.st_mtime);
    }
    closedir(d);
    return 1;
}
static int f_free(const char *p, unsigned long long *free_b, unsigned long long *total) {
    struct statvfs s;
    if (statvfs(p, &s) != 0) return 0;
    *free_b = (unsigned long long)s.f_bavail * s.f_frsize;
    *total = (unsigned long long)s.f_blocks * s.f_frsize;
    // Linux-сборка нужна только для тестов: так проверяется отказ при нехватке места
    const char *t = getenv("WB_TEST_FREE");
    if (t) *free_b = strtoull(t, NULL, 10);
    return 1;
}
static void local_tm(time_t t, struct tm *out) { localtime_r(&t, out); }
#endif

// ---------- пути ----------

// Абсолютный путь, приведённый к одному виду (на Windows через rs_path_norm). 0 — не абсолютный.
static int path_ok(const char *in, char *out, size_t n) {
#ifdef _WIN32
    return rs_path_norm(in, out, n);
#else
    if (!in || in[0] != '/' || strlen(in) >= n) return 0;
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    return 1;
#endif
}

static int pjoin(char *out, size_t n, const char *a, const char *b) {
    size_t la = strlen(a);
    int sep = la > 0 && a[la - 1] != SEP;
    int r = snprintf(out, n, "%s%s%s", a, sep ? (SEP == '\\' ? "\\" : "/") : "", b);
    return r > 0 && (size_t)r < n;
}

static const char *base_name_u(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    return b;
}

static int same_path(const char *a, const char *b) {
#ifdef _WIN32
    // Windows: регистр в путях не важен (сравниваем ASCII без регистра, остальное побайтно)
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
#else
    return !strcmp(a, b);
#endif
}

static unsigned fnv1a(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
#endif
        h = (h ^ c) * 16777619u;
    }
    return h;
}

// чья это папка: 1 — этого мира, 0 — другого мира или чужая, -1 — не существует
static int folder_owner(const char *dir, const char *world) {
    if (f_stat(dir, NULL, NULL) != 1) return -1;
    char mp[RS_PATH + 64];
    if (!pjoin(mp, sizeof mp, dir, MARKER)) return 0;
    fh_t h = f_open_read(mp);
    if (h == BAD_FH) return 0;
    char buf[RS_PATH + 4];
    long long got = f_read(h, buf, sizeof buf - 1);
    f_close(h);
    if (got <= 0) return 0;
    buf[got] = 0;
    char *nl = strpbrk(buf, "\r\n");
    if (nl) *nl = 0;
    return same_path(buf, world) ? 1 : 0;
}

// Папка копий мира. create: создать её (и метку). 1 — готово, 0 — ошибка (reason).
static int world_folder(const char *root, const char *world, int create, char *out, size_t n, const char **reason) {
    const char *name = base_name_u(world);
    char cand[RS_PATH + 300], label[300];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 0) snprintf(label, sizeof label, "%s", name);
        else snprintf(label, sizeof label, "%s (%08x)", name, fnv1a(world));
        if (!pjoin(cand, sizeof cand, root, label)) { *reason = "path"; return 0; }
        int own = folder_owner(cand, world);
        if (own == 1) break;
        if (own == -1) {
            if (!create) break; // ещё нет: копий нет, путь всё равно сообщаем
            if (!f_mkdir(root) || !f_mkdir(cand)) { *reason = "mkdir"; return 0; }
            char mp[RS_PATH + 400];
            pjoin(mp, sizeof mp, cand, MARKER);
            fh_t h = f_create(mp);
            if (h == BAD_FH) { *reason = "marker"; return 0; }
            int ok = f_write(h, world, strlen(world)) && f_write(h, "\n", 1);
            f_close(h);
            if (!ok) { *reason = "marker"; return 0; }
#ifdef _WIN32
            { wchar_t *w = wp(mp); if (w) { SetFileAttributesW(w, FILE_ATTRIBUTE_HIDDEN); free(w); } }
#endif
            break;
        }
        if (attempt == 1) { *reason = "folder_conflict"; return 0; } // хеш совпал у разных миров
    }
    if (strlen(cand) >= n) { *reason = "path"; return 0; }
    snprintf(out, n, "%s", cand);
    return 1;
}

// ---------- имена архивов ----------
// <ГГГГ-ММ-ДД_ЧЧ-ММ-СС>_<clean|hot>[_<n>].zip; kind: 'c' / 'h'
static int parse_copy_name(const char *s, char *kind, char *local, size_t ln) {
    int y, mo, d, h, mi, se, used = 0;
    if (sscanf(s, "%4d-%2d-%2d_%2d-%2d-%2d_%n", &y, &mo, &d, &h, &mi, &se, &used) != 6 || used != 20) return 0;
    const char *k = s + used;
    const char *rest;
    if (!strncmp(k, "clean", 5)) { *kind = 'c'; rest = k + 5; }
    else if (!strncmp(k, "hot", 3)) { *kind = 'h'; rest = k + 3; }
    else return 0;
    if (*rest == '_') {
        rest++;
        if (!(*rest >= '0' && *rest <= '9')) return 0;
        while (*rest >= '0' && *rest <= '9') rest++;
    }
    if (strcmp(rest, ".zip")) return 0;
    if (local) snprintf(local, ln, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, se);
    return 1;
}

typedef struct {
    char name[160];
    char kind;
    char local[24];
    unsigned long long bytes;
    long long mtime;
} copy_t;

typedef struct { copy_t *v; int n, cap; } copies_t;

static int collect_copy(void *ctx, const char *name, int isdir, unsigned long long size, long long mtime) {
    copies_t *c = ctx;
    char kind, local[24];
    if (isdir || strlen(name) >= sizeof c->v[0].name || !parse_copy_name(name, &kind, local, sizeof local)) return 1;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        copy_t *nv = realloc(c->v, sizeof(copy_t) * (size_t)cap);
        if (!nv) return 0;
        c->v = nv;
        c->cap = cap;
    }
    copy_t *x = &c->v[c->n++];
    snprintf(x->name, sizeof x->name, "%s", name);
    x->kind = kind;
    snprintf(x->local, sizeof x->local, "%s", local);
    x->bytes = size;
    x->mtime = mtime;
    return 1;
}

// новые первыми: по времени начала копии, при равенстве по имени (суффикс _2 новее)
static int copy_cmp(const void *a, const void *b) {
    const copy_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(y->name, x->name);
}

static int list_copies(const char *folder, copies_t *c) {
    c->v = NULL;
    c->n = c->cap = 0;
    if (f_stat(folder, NULL, NULL) != 1) return 1;
    if (!f_listdir(folder, collect_copy, c)) return 0;
    if (c->n > 1) qsort(c->v, (size_t)c->n, sizeof(copy_t), copy_cmp);
    return 1;
}

// ======================================================================
// запись zip
// ======================================================================

typedef struct {
    fh_t fh;
    unsigned long long pos;      // текущий размер архива
    unsigned long long csize;    // сжатый размер текущей записи
    unsigned long long entries;
    sbuf cd;                     // центральный каталог копится в памяти и пишется в конце
    tdefl_compressor *comp;
    int failed;
} zipw;

static void le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void le32(unsigned char *p, unsigned long v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void le64(unsigned char *p, unsigned long long v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }

static int zw_write(zipw *z, const void *buf, size_t n) {
    if (z->failed) return 0;
    if (!f_write(z->fh, buf, n)) { z->failed = 1; return 0; }
    z->pos += n;
    return 1;
}

static mz_bool zw_put(const void *buf, int len, void *user) {
    zipw *z = user;
    if (!zw_write(z, buf, (size_t)len)) return MZ_FALSE;
    z->csize += (unsigned long long)len;
    return MZ_TRUE;
}

static void dos_time(long long t, unsigned *date, unsigned *tm_) {
    struct tm lt;
    local_tm((time_t)t, &lt);
    if (lt.tm_year < 80) { *date = (1 << 5) | 1; *tm_ = 0; return; } // раньше 1980 zip не умеет
    *date = (unsigned)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    *tm_ = (unsigned)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
}

// запись центрального каталога
static void zw_central(zipw *z, const char *name, int isdir, int method, unsigned date, unsigned tm_, unsigned long crc,
                       unsigned long long csize, unsigned long long usize, unsigned long long off) {
    unsigned char h[46], ex[28];
    size_t nl = strlen(name), el = 0;
    int big_u = usize >= 0xFFFFFFFFULL, big_c = csize >= 0xFFFFFFFFULL, big_o = off >= 0xFFFFFFFFULL;
    if (big_u || big_c || big_o) {
        // поле ZIP64 в центральном каталоге содержит только переполненные значения, в этом порядке
        el = 4;
        if (big_u) { le64(ex + el, usize); el += 8; }
        if (big_c) { le64(ex + el, csize); el += 8; }
        if (big_o) { le64(ex + el, off); el += 8; }
        le16(ex, 0x0001);
        le16(ex + 2, (unsigned)(el - 4));
    }
    int z64 = el > 0;
    le32(h, 0x02014b50);
    le16(h + 4, 45);                    // создано: версия 4.5, MS-DOS
    le16(h + 6, z64 ? 45 : (isdir ? 10 : 20));
    le16(h + 8, 0x0800);                // имена в UTF-8
    le16(h + 10, (unsigned)method);
    le16(h + 12, tm_);
    le16(h + 14, date);
    le32(h + 16, crc);
    le32(h + 20, big_c ? 0xFFFFFFFFUL : (unsigned long)csize);
    le32(h + 24, big_u ? 0xFFFFFFFFUL : (unsigned long)usize);
    le16(h + 28, (unsigned)nl);
    le16(h + 30, (unsigned)el);
    le16(h + 32, 0);
    le16(h + 34, 0);
    le16(h + 36, 0);
    le32(h + 38, isdir ? 0x10 : 0x20);  // атрибуты MS-DOS: каталог / архивный
    le32(h + 42, big_o ? 0xFFFFFFFFUL : (unsigned long)off);
    if (!sb_putn(&z->cd, (const char *)h, sizeof h) || !sb_putn(&z->cd, name, nl) || (el && !sb_putn(&z->cd, (const char *)ex, el)))
        z->failed = 1;
    z->entries++;
}

typedef struct job job_t;
static int job_progress(job_t *j, unsigned long long add);

// Файлы регионов (.mca, .mcr, .mcc): чанки внутри уже сжаты игрой, сжимаются только нули,
// которыми чанки выровнены на секторы по 4 КБ. Режим RLE (только серии одинаковых байтов)
// даёт на них то же сжатие, что обычный deflate, но примерно вдвое быстрее (замер: 69 против
// 31 МБ/с на одном ядре тестовой машины).
static int region_file(const char *name) {
    size_t l = strlen(name);
    if (l < 4) return 0;
    const char *e = name + l - 4;
    return !strcmp(e, ".mca") || !strcmp(e, ".mcr") || !strcmp(e, ".mcc");
}

// Добавить запись. Для файла читается не больше size байт (размер на момент обхода): так заголовок
// всегда соответствует данным, даже если игра дописывает файл во время горячей копии.
// Возвращает 1 — записано, 0 — ошибка архива, -1 — файл не прочитать (решает вызывающий).
static int zw_add(zipw *z, job_t *j, const char *name, int isdir, const char *src, unsigned long long size, long long mtime,
                  unsigned char *buf) {
    unsigned date, tm_;
    dos_time(mtime, &date, &tm_);
    unsigned long long off = z->pos;
    size_t nl = strlen(name);
    if (isdir) {
        unsigned char h[30];
        le32(h, 0x04034b50);
        le16(h + 4, 10);
        le16(h + 6, 0x0800);
        le16(h + 8, 0);
        le16(h + 10, tm_);
        le16(h + 12, date);
        le32(h + 14, 0);
        le32(h + 18, 0);
        le32(h + 22, 0);
        le16(h + 26, (unsigned)nl);
        le16(h + 28, 0);
        if (!zw_write(z, h, sizeof h) || !zw_write(z, name, nl)) return 0;
        zw_central(z, name, 1, 0, date, tm_, 0, 0, 0, off);
        return !z->failed;
    }
    fh_t in = f_open_read(src);
    if (in == BAD_FH) return -1;
    int z64 = size >= Z64_FILE;
    unsigned char h[30 + 20];
    le32(h, 0x04034b50);
    le16(h + 4, z64 ? 45 : 20);
    le16(h + 6, 0x0800);
    le16(h + 8, 8); // deflate
    le16(h + 10, tm_);
    le16(h + 12, date);
    le32(h + 14, 0); // CRC и размеры записываются после сжатия
    le32(h + 18, z64 ? 0xFFFFFFFFUL : 0);
    le32(h + 22, z64 ? 0xFFFFFFFFUL : 0);
    le16(h + 26, (unsigned)nl);
    le16(h + 28, z64 ? 20 : 0);
    if (!zw_write(z, h, 30) || !zw_write(z, name, nl)) { f_close(in); return 0; }
    if (z64) {
        unsigned char ex[20] = { 0 };
        le16(ex, 0x0001);
        le16(ex + 2, 16);
        if (!zw_write(z, ex, sizeof ex)) { f_close(in); return 0; }
    }
    z->csize = 0;
    int flags = (int)tdefl_create_comp_flags_from_zip_params(DEFLATE_LEVEL, -15, region_file(name) ? MZ_RLE : MZ_DEFAULT_STRATEGY);
    if (tdefl_init(z->comp, zw_put, z, flags) != TDEFL_STATUS_OKAY) { f_close(in); z->failed = 1; return 0; }
    unsigned long crc = MZ_CRC32_INIT;
    unsigned long long done = 0;
    int read_fail = 0;
    while (done < size) {
        size_t want = size - done > CHUNK ? CHUNK : (size_t)(size - done);
        long long got = f_read(in, buf, want);
        if (got < 0) { read_fail = 1; break; }
        if (got == 0) break; // файл стал короче: пишем то, что есть
        crc = mz_crc32(crc, buf, (size_t)got);
        if (tdefl_compress_buffer(z->comp, buf, (size_t)got, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) { z->failed = 1; break; }
        done += (unsigned long long)got;
        if (!job_progress(j, (unsigned long long)got)) { z->failed = 1; break; } // отмена
    }
    f_close(in);
    if (z->failed) return 0;
    if (read_fail) {
        // откатываем начатую запись: архив продолжается с её начала
        if (!f_seek(z->fh, off)) { z->failed = 1; return 0; }
        z->pos = off;
        return -1;
    }
    if (tdefl_compress_buffer(z->comp, NULL, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) { z->failed = 1; return 0; }
    // вернуться и дописать CRC и размеры в локальный заголовок
    unsigned long long end = z->pos;
    unsigned char fix[12];
    le32(fix, crc);
    le32(fix + 4, z64 ? 0xFFFFFFFFUL : (unsigned long)z->csize);
    le32(fix + 8, z64 ? 0xFFFFFFFFUL : (unsigned long)done);
    if (!f_seek(z->fh, off + 14) || !f_write(z->fh, fix, sizeof fix)) { z->failed = 1; return 0; }
    if (z64) {
        unsigned char s[16];
        le64(s, done);
        le64(s + 8, z->csize);
        if (!f_seek(z->fh, off + 30 + nl + 4) || !f_write(z->fh, s, sizeof s)) { z->failed = 1; return 0; }
    } else if (z->csize >= 0xFFFFFFFFULL) {
        z->failed = 1; // не бывает: сжатый файл меньше Z64_FILE не может вырасти до 4 ГБ
        return 0;
    }
    if (!f_seek(z->fh, end)) { z->failed = 1; return 0; }
    zw_central(z, name, 0, 8, date, tm_, crc, z->csize, done, off);
    return !z->failed;
}

static int zw_finish(zipw *z) {
    unsigned long long cd_off = z->pos, cd_size = z->cd.n;
    if (z->cd.failed || !zw_write(z, z->cd.p ? z->cd.p : "", z->cd.n)) return 0;
    int z64 = z->entries >= 0xFFFF || cd_off >= 0xFFFFFFFFULL || cd_size >= 0xFFFFFFFFULL;
    if (z64) {
        unsigned char r[56], loc[20];
        unsigned long long rec_off = z->pos;
        le32(r, 0x06064b50);
        le64(r + 4, 44);
        le16(r + 12, 45);
        le16(r + 14, 45);
        le32(r + 16, 0);
        le32(r + 20, 0);
        le64(r + 24, z->entries);
        le64(r + 32, z->entries);
        le64(r + 40, cd_size);
        le64(r + 48, cd_off);
        le32(loc, 0x07064b50);
        le32(loc + 4, 0);
        le64(loc + 8, rec_off);
        le32(loc + 16, 1);
        if (!zw_write(z, r, sizeof r) || !zw_write(z, loc, sizeof loc)) return 0;
    }
    unsigned char e[22];
    le32(e, 0x06054b50);
    le16(e + 4, 0);
    le16(e + 6, 0);
    le16(e + 8, z->entries >= 0xFFFF ? 0xFFFF : (unsigned)z->entries);
    le16(e + 10, z->entries >= 0xFFFF ? 0xFFFF : (unsigned)z->entries);
    le32(e + 12, cd_size >= 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (unsigned long)cd_size);
    le32(e + 16, cd_off >= 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (unsigned long)cd_off);
    le16(e + 20, 0);
    return zw_write(z, e, sizeof e);
}

// ======================================================================
// задание копирования (отдельный поток)
// ======================================================================

typedef struct { char *rel; int isdir; unsigned long long size; long long mtime; } entry_t;

struct job {
    char seq[32], world[RS_PATH], root[RS_PATH], kind[8];
    int if_changed;
    entry_t *e;
    int n, cap;
    unsigned long long total, done;
    long long max_mtime;
    unsigned long long last_emit_ms;
    int last_pct;
    int walk_failed;
};

static volatile int g_busy = 0;
static volatile int g_cancel = 0;

int backup_busy(void) { return g_busy; }

static void job_line(job_t *j, const char *kind, const char *reason, const char *detail) {
    sbuf l = {0};
    sb_put(&l, kind);
    sb_field(&l, "seq", j->seq);
    if (reason) sb_field(&l, "reason", reason);
    if (detail) sb_field(&l, "detail", detail);
    sb_emit(&l);
}

static int job_progress(job_t *j, unsigned long long add) {
    j->done += add;
    if (g_cancel) return 0;
    int pct = j->total ? (int)(j->done * 100 / j->total) : 100;
    unsigned long long t = now_ms();
    if (pct != j->last_pct && t - j->last_emit_ms >= 250) {
        j->last_pct = pct;
        j->last_emit_ms = t;
        sbuf l = {0};
        sb_put(&l, "progress");
        sb_field(&l, "seq", j->seq);
        sb_field_int(&l, "pct", pct > 99 ? 99 : pct);
        sb_field_int(&l, "bytes", (long long)j->done);
        sb_emit(&l);
    }
    return 1;
}

typedef struct { job_t *j; const char *dir; const char *rel; int depth; } walk_t;
static int walk_dir(job_t *j, const char *dir, const char *rel, int depth);

static int job_push(job_t *j, const char *rel, int isdir, unsigned long long size, long long mtime) {
    if (j->n == j->cap) {
        int cap = j->cap ? j->cap * 2 : 1024;
        entry_t *ne = realloc(j->e, sizeof(entry_t) * (size_t)cap);
        if (!ne) return 0;
        j->e = ne;
        j->cap = cap;
    }
    size_t l = strlen(rel);
    char *r = malloc(l + 1);
    if (!r) return 0;
    memcpy(r, rel, l + 1);
    j->e[j->n].rel = r;
    j->e[j->n].isdir = isdir;
    j->e[j->n].size = size;
    j->e[j->n].mtime = mtime;
    j->n++;
    return 1;
}

static int walk_cb(void *ctx, const char *name, int isdir, unsigned long long size, long long mtime) {
    walk_t *w = ctx;
    job_t *j = w->j;
    if (w->depth == 0 && !isdir && !strcmp(name, "session.lock")) return 1; // занят игрой и не нужен
    char rel[RS_PATH];
    int r = w->rel[0] ? snprintf(rel, sizeof rel, "%s/%s", w->rel, name) : snprintf(rel, sizeof rel, "%s", name);
    if (r <= 0 || (size_t)r >= sizeof rel) { j->walk_failed = 1; return 0; }
    if (!job_push(j, rel, isdir, isdir ? 0 : size, mtime)) { j->walk_failed = 1; return 0; }
    if (!isdir) {
        j->total += size;
        if (mtime > j->max_mtime) j->max_mtime = mtime;
        return 1;
    }
    char sub[RS_PATH * 2];
    if (!pjoin(sub, sizeof sub, w->dir, name)) { j->walk_failed = 1; return 0; }
    return walk_dir(j, sub, rel, w->depth + 1);
}

static int walk_dir(job_t *j, const char *dir, const char *rel, int depth) {
    if (depth > 64) { j->walk_failed = 1; return 0; }
    walk_t w = { j, dir, rel, depth };
    if (!f_listdir(dir, walk_cb, &w)) { j->walk_failed = 1; return 0; }
    return !j->walk_failed;
}

static void job_free(job_t *j) {
    for (int i = 0; i < j->n; i++) free(j->e[i].rel);
    free(j->e);
    free(j);
}

static void human_detail(char *out, size_t n, unsigned long long need, unsigned long long have) {
    snprintf(out, n, "need=%llu free=%llu", need, have);
}

// брошенные *.zip.tmp прошлых попыток (хелпер упал или его завершили посреди копии)
typedef struct { const char *folder; } tmp_ctx;
static int tmp_cb(void *ctx, const char *name, int isdir, unsigned long long size, long long mtime) {
    (void)size; (void)mtime;
    size_t l = strlen(name);
    char kind;
    if (isdir || l < 8 || strcmp(name + l - 4, ".tmp")) return 1;
    char base[200];
    if (l - 4 >= sizeof base) return 1;
    memcpy(base, name, l - 4);
    base[l - 4] = 0;
    if (!parse_copy_name(base, &kind, NULL, 0)) return 1; // удаляем только свои
    char full[RS_PATH + 500];
    if (pjoin(full, sizeof full, ((tmp_ctx *)ctx)->folder, name)) f_delete(full);
    return 1;
}
static void cleanup_tmp(const char *folder) {
    tmp_ctx c = { folder };
    f_listdir(folder, tmp_cb, &c);
}

static THREAD_RET backup_thread(void *arg) {
    job_t *j = arg;
    unsigned long long t0 = now_ms();
    long long start = (long long)time(NULL);
    char folder[RS_PATH + 300], tmp[RS_PATH + 520], final_[RS_PATH + 500], detail[160];
    const char *reason = NULL;
    zipw z;
    memset(&z, 0, sizeof z);
    z.fh = BAD_FH;
    unsigned char *buf = NULL;
    int skipped = 0;

    // 1. обход мира: список файлов, общий размер, последнее изменение
    if (!walk_dir(j, j->world, "", 0)) { job_line(j, "backup_error", "walk", NULL); goto done; }
    if (!world_folder(j->root, j->world, 1, folder, sizeof folder, &reason)) { job_line(j, "backup_error", reason, NULL); goto done; }

    // 2. мир не менялся с прошлой копии — не копируем (для чистой сравниваем с чистыми)
    if (j->if_changed) {
        copies_t c;
        if (list_copies(folder, &c)) {
            for (int i = 0; i < c.n; i++) {
                if (!strcmp(j->kind, "clean") && c.v[i].kind != 'c') continue;
                if (c.v[i].mtime > j->max_mtime) { // строго: изменение в ту же секунду считаем новым
                    sbuf l = {0};
                    sb_put(&l, "backup_skip");
                    sb_field(&l, "seq", j->seq);
                    sb_field(&l, "reason", "unchanged");
                    sb_field(&l, "last", c.v[i].name);
                    sb_emit(&l);
                    free(c.v);
                    goto done;
                }
                break; // смотрим только самую свежую подходящую
            }
            free(c.v);
        }
    }

    // 3. место
    {
        unsigned long long fr = 0, tot = 0;
        if (f_free(folder, &fr, &tot) && fr < SPACE_NEED(j->total)) {
            human_detail(detail, sizeof detail, SPACE_NEED(j->total), fr);
            job_line(j, "backup_error", "no_space", detail);
            goto done;
        }
    }

    // 4. имя архива; незаконченные .tmp прошлых попыток удаляем (они только наши: пишет один поток)
    {
        struct tm lt;
        local_tm((time_t)start, &lt);
        char base[64];
        snprintf(base, sizeof base, "%04d-%02d-%02d_%02d-%02d-%02d_%s", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min, lt.tm_sec, j->kind);
        for (int k = 1; k < 100; k++) {
            char nm[96];
            if (k == 1) snprintf(nm, sizeof nm, "%s.zip", base);
            else snprintf(nm, sizeof nm, "%s_%d.zip", base, k);
            pjoin(final_, sizeof final_, folder, nm);
            if (f_stat(final_, NULL, NULL) == -1) break;
        }
        snprintf(tmp, sizeof tmp, "%s.tmp", final_);
    }
    cleanup_tmp(folder);

    sbuf l = {0};
    sb_put(&l, "backup_start");
    sb_field(&l, "seq", j->seq);
    sb_field(&l, "file", final_);
    sb_field_int(&l, "bytes", (long long)j->total);
    sb_field_int(&l, "files", j->n);
    sb_emit(&l);

    // 5. архив
    buf = malloc(CHUNK);
    z.comp = malloc(sizeof(tdefl_compressor));
    z.fh = f_create(tmp);
    if (!buf || !z.comp) { job_line(j, "backup_error", "memory", NULL); goto fail; }
    if (z.fh == BAD_FH) { job_line(j, "backup_error", "create", tmp); goto fail; }
    {
        const char *wname = base_name_u(j->world);
        char name[RS_PATH + 300], src[RS_PATH * 2];
        long long wmt = start;
        f_stat(j->world, NULL, &wmt);
        snprintf(name, sizeof name, "%s/", wname);
        if (zw_add(&z, j, name, 1, NULL, 0, wmt, buf) != 1) { job_line(j, "backup_error", g_cancel ? "cancelled" : "write", NULL); goto fail; }
        for (int i = 0; i < j->n; i++) {
            entry_t *e = &j->e[i];
            snprintf(name, sizeof name, "%s/%s%s", wname, e->rel, e->isdir ? "/" : "");
            // путь на диске: тот же относительный путь с разделителями системы
            char relsys[RS_PATH];
            snprintf(relsys, sizeof relsys, "%s", e->rel);
            for (char *q = relsys; *q; q++) if (*q == '/') *q = SEP;
            pjoin(src, sizeof src, j->world, relsys);
            int r = zw_add(&z, j, name, e->isdir, src, e->size, e->mtime, buf);
            if (r == 1) continue;
            if (r == -1 && !strcmp(j->kind, "hot")) { skipped++; continue; } // горячая: файл занят — пропускаем
            if (r == -1) { job_line(j, "backup_error", "read", e->rel); goto fail; }
            job_line(j, "backup_error", g_cancel ? "cancelled" : "write", NULL);
            goto fail;
        }
    }
    if (!zw_finish(&z) || !f_truncate(z.fh, z.pos) || !f_flush(z.fh)) { job_line(j, "backup_error", "write", NULL); goto fail; }
    f_set_mtime(z.fh, start); // время архива = начало копии
    f_close(z.fh);
    z.fh = BAD_FH;
    if (!f_rename(tmp, final_)) { job_line(j, "backup_error", "rename", NULL); goto fail; }
    {
        sbuf d = {0};
        sb_put(&d, "backup_done");
        sb_field(&d, "seq", j->seq);
        sb_field(&d, "file", final_);
        sb_field_int(&d, "bytes", (long long)z.pos);
        sb_field_int(&d, "src_bytes", (long long)j->total);
        sb_field_int(&d, "files", j->n);
        sb_field_int(&d, "skipped", skipped);
        sb_field_int(&d, "ms", (long long)(now_ms() - t0));
        sb_emit(&d);
    }
    goto done;

fail:
    if (z.fh != BAD_FH) f_close(z.fh);
    f_delete(tmp);
done:
    free(buf);
    free(z.comp);
    sb_free(&z.cd);
    job_free(j);
    g_busy = 0;
    THREAD_RETURN;
}

// ======================================================================
// команды
// ======================================================================

static void cmd_backup(char **a, int n) {
    // backup <seq> <путь мира> <папка копий> <clean|hot> <if_changed 0|1>
    const char *seq = n > 0 ? a[0] : "";
    if (n < 5 || (strcmp(a[3], "clean") && strcmp(a[3], "hot"))) {
        sbuf l = {0}; sb_put(&l, "backup_error"); sb_field(&l, "seq", seq); sb_field(&l, "reason", "bad_args"); sb_emit(&l);
        return;
    }
    if (g_busy) {
        sbuf l = {0}; sb_put(&l, "backup_error"); sb_field(&l, "seq", seq); sb_field(&l, "reason", "busy"); sb_emit(&l);
        return;
    }
    job_t *j = calloc(1, sizeof(job_t));
    if (!j) { emit_error("memory", "backup"); return; }
    snprintf(j->seq, sizeof j->seq, "%s", seq);
    snprintf(j->kind, sizeof j->kind, "%s", a[3]);
    j->if_changed = !strcmp(a[4], "1");
    j->last_pct = -1;
    if (!path_ok(a[1], j->world, sizeof j->world) || f_stat(j->world, NULL, NULL) != 1) {
        job_line(j, "backup_error", "no_world", NULL);
        job_free(j);
        return;
    }
    if (!path_ok(a[2], j->root, sizeof j->root)) {
        job_line(j, "backup_error", "bad_root", NULL);
        job_free(j);
        return;
    }
    g_busy = 1;
    g_cancel = 0;
    if (!thread_start(backup_thread, j)) {
        g_busy = 0;
        job_line(j, "backup_error", "thread", NULL);
        job_free(j);
    }
}

static int resolve_folder(const char *seq, const char *root_in, const char *world_in, char *folder, size_t n) {
    char root[RS_PATH], world[RS_PATH];
    const char *reason = NULL;
    if (!path_ok(root_in, root, sizeof root) || !path_ok(world_in, world, sizeof world)) reason = "bad_path";
    else if (!world_folder(root, world, 0, folder, n, &reason)) {}
    else return 1;
    sbuf l = {0};
    sb_put(&l, "list_error");
    sb_field(&l, "seq", seq);
    sb_field(&l, "reason", reason);
    sb_emit(&l);
    return 0;
}

static void cmd_list(char **a, int n) {
    // list <seq> <папка копий> <путь мира>
    if (n < 3) { emit_error("bad_args", "list"); return; }
    char folder[RS_PATH + 300];
    if (!resolve_folder(a[0], a[1], a[2], folder, sizeof folder)) return;
    copies_t c;
    if (!list_copies(folder, &c)) {
        sbuf l = {0}; sb_put(&l, "list_error"); sb_field(&l, "seq", a[0]); sb_field(&l, "reason", "read"); sb_emit(&l);
        return;
    }
    for (int i = 0; i < c.n; i++) {
        char full[RS_PATH + 500];
        pjoin(full, sizeof full, folder, c.v[i].name);
        sbuf l = {0};
        sb_put(&l, "copy");
        sb_field(&l, "seq", a[0]);
        sb_field(&l, "file", full);
        sb_field(&l, "kind", c.v[i].kind == 'c' ? "clean" : "hot");
        sb_field_int(&l, "bytes", (long long)c.v[i].bytes);
        sb_field_int(&l, "time", c.v[i].mtime);
        sb_field(&l, "local", c.v[i].local);
        sb_emit(&l);
    }
    sbuf l = {0};
    sb_put(&l, "list_done");
    sb_field(&l, "seq", a[0]);
    sb_field_int(&l, "count", c.n);
    sb_field(&l, "folder", folder);
    sb_emit(&l);
    free(c.v);
}

static void cmd_prune(char **a, int n) {
    // prune <seq> <папка копий> <путь мира> <N чистых> <M горячих> [лимит МБ, 0 — нет]
    if (n < 5) { emit_error("bad_args", "prune"); return; }
    int keep_c = atoi(a[3]), keep_h = atoi(a[4]);
    long long limit = n > 5 ? atoll(a[5]) * 1024LL * 1024LL : 0;
    if (keep_c < 1) keep_c = 1; // последнюю чистую копию не удаляем никогда
    if (keep_h < 0) keep_h = 0;
    char folder[RS_PATH + 300];
    if (!resolve_folder(a[0], a[1], a[2], folder, sizeof folder)) return;
    copies_t c;
    if (!list_copies(folder, &c)) { emit_error("read", "prune"); return; }
    char *del = calloc((size_t)(c.n ? c.n : 1), 1);
    int seen_c = 0, seen_h = 0, newest_clean = -1;
    unsigned long long total = 0;
    for (int i = 0; i < c.n && del; i++) {
        if (c.v[i].kind == 'c') { if (newest_clean < 0) newest_clean = i; if (++seen_c > keep_c) del[i] = 1; }
        else if (++seen_h > keep_h) del[i] = 1;
        if (!del[i]) total += c.v[i].bytes;
    }
    // лимит места: удалять самые старые из оставшихся, кроме последней чистой
    for (int i = c.n - 1; del && limit > 0 && (long long)total > limit && i >= 0; i--) {
        if (del[i] || i == newest_clean) continue;
        del[i] = 1;
        total -= c.v[i].bytes;
    }
    int count = 0;
    unsigned long long freed = 0;
    for (int i = 0; i < c.n && del; i++) {
        if (!del[i]) continue;
        char full[RS_PATH + 500];
        pjoin(full, sizeof full, folder, c.v[i].name);
        sbuf l = {0};
        sb_put(&l, "removed");
        sb_field(&l, "seq", a[0]);
        sb_field(&l, "file", full);
        if (f_delete(full)) { count++; freed += c.v[i].bytes; }
        else sb_field(&l, "error", "delete");
        sb_emit(&l);
    }
    sbuf l = {0};
    sb_put(&l, "pruned");
    sb_field(&l, "seq", a[0]);
    sb_field_int(&l, "count", count);
    sb_field_int(&l, "freed", (long long)freed);
    sb_emit(&l);
    free(del);
    free(c.v);
}

static void cmd_space(char **a, int n) {
    // space <seq> <путь>: свободное место на диске пути (или ближайшей существующей папки выше)
    if (n < 2) { emit_error("bad_args", "space"); return; }
    char p[RS_PATH];
    unsigned long long fr = 0, tot = 0;
    int ok = path_ok(a[1], p, sizeof p);
    while (ok && f_stat(p, NULL, NULL) != 1) {
        char *s = strrchr(p, SEP);
        if (!s) { ok = 0; break; }
        if (s == p || (s == p + 2 && p[1] == ':')) s[1] = 0; // корень "/" или "C:\"
        else *s = 0;
        if (f_stat(p, NULL, NULL) != 1 && (!strcmp(p, "/") || (strlen(p) == 3 && p[1] == ':'))) { ok = 0; break; }
    }
    sbuf l = {0};
    if (ok && f_free(p, &fr, &tot)) {
        sb_put(&l, "space");
        sb_field(&l, "seq", a[0]);
        sb_field_int(&l, "free", (long long)fr);
        sb_field_int(&l, "total", (long long)tot);
    } else {
        sb_put(&l, "space_error");
        sb_field(&l, "seq", a[0]);
    }
    sb_emit(&l);
}

static void cmd_paths(void) {
    // папка копий по умолчанию: <Документы>\Porthole Backups (хелпер её не создаёт)
    char docs[RS_PATH] = "", out[RS_PATH + 32];
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, w) == S_OK)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, docs, sizeof docs, NULL, NULL);
#else
    const char *h = getenv("HOME");
    if (h) snprintf(docs, sizeof docs, "%s/Documents", h);
#endif
    sbuf l = {0};
    sb_put(&l, "paths");
    if (docs[0] && pjoin(out, sizeof out, docs, "Porthole Backups")) sb_field(&l, "backups", out);
    else sb_field(&l, "backups", "");
    sb_emit(&l);
}

// 1 — команда из этого файла (обработана), 0 — не наша
int backup_command(const char *cmd, int n, char **a) {
    if (!strcmp(cmd, "backup")) cmd_backup(a, n);
    else if (!strcmp(cmd, "list")) cmd_list(a, n);
    else if (!strcmp(cmd, "prune")) cmd_prune(a, n);
    else if (!strcmp(cmd, "space")) cmd_space(a, n);
    else if (!strcmp(cmd, "paths")) cmd_paths();
    else return 0;
    return 1;
}

// при выходе хелпера: попросить копию остановиться и дождаться (до 10 с), чтобы удалился .tmp
void backup_shutdown(void) {
    if (!g_busy) return;
    g_cancel = 1;
    for (int i = 0; i < 100 && g_busy; i++) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
}
