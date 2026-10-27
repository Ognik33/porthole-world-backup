// resolve.c — какой мир Minecraft открыт на данном порту (шаг 2 плана).
//
// Цепочка: порт → PID процесса, который его слушает → папка игры → открытый мир в <папка>\saves.
//   Папка игры: --gameDir из командной строки (обычный лаунчер), относительный — от рабочей папки
//   процесса; нет --gameDir (MultiMC, Prism передают параметры через stdin) — рабочая папка процесса.
//   Открытый мир: session.lock занят игрой (новые версии) → это он; иначе session.lock изменён после
//   запуска процесса (старые, например 1.7.10) → это он; иначе спросить человека.
//
// Безопасность: из командной строки чужого процесса берётся только значение --gameDir и маркеры
// лаунчера. Командная строка целиком никогда не выводится: обычный лаунчер передаёт там токен входа.
//
// Файл в двух частях: переносимая логика (rs_*), которую проверяет tests/resolve_test.c на Linux,
// и вызовы Windows. Код Windows перенесён из лабы (lab_probe.c: gamedir, sessionlock),
// проверенной на Porthole 3.0.2 с 26.3 (session.lock занят) и 1.7.10 (не занят, но изменён).
#include "wb.h"

#define MAX_WORLDS 512 // миров в одной папке saves больше не смотрим
#define MAX_CAND 20    // кандидатов в вопросе человеку
#define MTIME_SLACK 2  // с: session.lock может быть записан чуть раньше, чем округлилось время запуска

// ======================================================================
// переносимая логика
// ======================================================================

static int is_sep(char c) { return c == '\\' || c == '/'; }
static int is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static const char *iprefix(const char *s, const char *pre) {
    size_t n = strlen(pre);
    char tmp[64];
    if (n >= sizeof tmp || strlen(s) < n) return NULL;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    return ieq(tmp, pre) ? s + n : NULL;
}

// абсолютный путь Windows: "C:\..." или UNC "\\server\share..."
int rs_is_abs(const char *p) {
    if (!p) return 0;
    if (is_letter(p[0]) && p[1] == ':' && is_sep(p[2])) return 1;
    if (is_sep(p[0]) && is_sep(p[1]) && p[2] && !is_sep(p[2])) return 1;
    return 0;
}

// Привести абсолютный путь к одному виду: разделители '\', без повторов, без "." и "..",
// без завершающего '\' (кроме корня диска "C:\"). Буква диска — заглавная. 0 — путь не абсолютный
// или не влез в out.
int rs_path_norm(const char *in, char *out, size_t n) {
    if (!rs_is_abs(in) || n < 8) return 0;
    size_t o = 0;
    const char *p;
    if (in[1] == ':') {
        out[0] = (char)(in[0] >= 'a' ? in[0] - 32 : in[0]);
        out[1] = ':';
        out[2] = '\\';
        o = 3;
        p = in + 3;
    } else {
        // UNC: корень "\\server\share" неделим, ".." выше него не поднимается
        out[0] = out[1] = '\\';
        o = 2;
        p = in + 2;
        for (int part = 0; part < 2; part++) {
            while (is_sep(*p)) p++;
            if (!*p) return 0;
            if (part) out[o++] = '\\';
            while (*p && !is_sep(*p)) {
                if (o + 2 >= n) return 0;
                out[o++] = *p++;
            }
        }
    }
    size_t root = o;
    size_t stack[256];
    int depth = 0;
    while (*p) {
        while (is_sep(*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !is_sep(*p)) p++;
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (depth) o = stack[--depth];
            continue;
        }
        if (depth == 256) return 0;
        stack[depth++] = o;
        int sep = !(o == root && out[root - 1] == '\\'); // после "C:\" разделитель уже есть
        if (o + len + 2 >= n) return 0;
        if (sep) out[o++] = '\\';
        memcpy(out + o, s, len);
        o += len;
    }
    out[o] = 0;
    return 1;
}

void rs_parse_args(int argc, char **argv, rs_args *a) {
    a->gamedir = NULL;
    a->source = NULL;
    a->launcher = "unknown";
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (!s) continue;
        // только маркеры, не значения: соседний аргумент может быть токеном входа
        if (strstr(s, "org.prismlauncher") || strstr(s, "PrismLauncher")) a->launcher = "prism";
        else if (strstr(s, "org.multimc") || strstr(s, "NewLaunch")) a->launcher = "multimc";
        else if (ieq(s, "--accessToken") && !strcmp(a->launcher, "unknown")) a->launcher = "vanilla";
        const char *v;
        if (ieq(s, "--gameDir") && i + 1 < argc && argv[i + 1]) {
            a->gamedir = argv[i + 1];
            a->source = "gameDir";
            i++;
        } else if ((v = iprefix(s, "--gameDir=")) != NULL) {
            a->gamedir = v;
            a->source = "gameDir";
        } else if ((v = iprefix(s, "-Dminecraft.applet.TargetDirectory=")) != NULL) {
            // старые лаунчеры; --gameDir, если есть, важнее
            if (!a->source || strcmp(a->source, "gameDir")) {
                a->gamedir = v;
                a->source = "targetdir";
            }
        }
    }
    if (a->gamedir && !*a->gamedir) { a->gamedir = NULL; a->source = NULL; }
}

int rs_game_dir(const rs_args *a, const char *cwd, char *out, size_t n, const char **how) {
    char tmp[RS_PATH * 2];
    int has_cwd = cwd && rs_is_abs(cwd);
    if (!a->gamedir) {
        *how = "cwd";
        return has_cwd && rs_path_norm(cwd, out, n);
    }
    const char *g = a->gamedir;
    int gd = !strcmp(a->source, "gameDir");
    if (rs_is_abs(g)) {
        *how = gd ? "gameDir" : "targetdir";
        return rs_path_norm(g, out, n);
    }
    *how = gd ? "gameDir+cwd" : "targetdir+cwd";
    if (is_letter(g[0]) && g[1] == ':') {
        // "D:folder": от текущей папки того диска; у чужого процесса она известна, только если это
        // диск его рабочей папки, иначе — от корня диска
        if (has_cwd && (cwd[0] | 32) == (g[0] | 32)) snprintf(tmp, sizeof tmp, "%s\\%s", cwd, g + 2);
        else snprintf(tmp, sizeof tmp, "%c:\\%s", g[0], g + 2);
    } else if (!has_cwd) {
        return 0;
    } else if (is_sep(g[0])) {
        // "\folder": от корня диска рабочей папки (или корня её UNC-ресурса)
        if (cwd[1] == ':') snprintf(tmp, sizeof tmp, "%c:%s", cwd[0], g);
        else {
            char root[RS_PATH];
            if (!rs_path_norm(cwd, root, sizeof root)) return 0;
            // корень UNC = первые два компонента после "\\"
            char *q = root + 2;
            q = strchr(q, '\\');
            if (q) { q = strchr(q + 1, '\\'); if (q) *q = 0; }
            snprintf(tmp, sizeof tmp, "%s%s", root, g);
        }
    } else {
        snprintf(tmp, sizeof tmp, "%s\\%s", cwd, g);
    }
    return rs_path_norm(tmp, out, n);
}

int rs_pick(const rs_world *w, int n, long long start, int *picked, int *cand, int max_cand, int *ncand) {
    *picked = -1;
    *ncand = 0;
    int locked = 0, last_locked = -1, fresh = 0, last_fresh = -1;
    for (int i = 0; i < n; i++) {
        if (w[i].locked == 1) { locked++; last_locked = i; }
        if (start > 0 && w[i].lock_mtime >= start - MTIME_SLACK) { fresh++; last_fresh = i; }
    }
    if (locked == 1) { *picked = last_locked; return RS_LOCK; }
    if (locked == 0 && fresh == 1) { *picked = last_fresh; return RS_MTIME; }
    // спросить: из занятых, если их несколько; иначе из изменённых после запуска; иначе из всех
    int mode = locked > 1 ? 1 : (locked == 0 && fresh > 1) ? 2 : 3;
    for (int i = 0; i < n; i++) {
        int take = mode == 1 ? w[i].locked == 1 : mode == 2 ? w[i].lock_mtime >= start - MTIME_SLACK : 1;
        if (!take) continue;
        // вставка по убыванию lock_mtime: самые свежие первыми; лишние старые отбрасываются
        int k = *ncand < max_cand ? (*ncand)++ : max_cand;
        while (k > 0 && w[cand[k - 1]].lock_mtime < w[i].lock_mtime) {
            if (k < max_cand) cand[k] = cand[k - 1];
            k--;
        }
        if (k < max_cand) cand[k] = i;
    }
    return RS_ASK;
}

// ======================================================================
// Windows
// ======================================================================
#ifdef _WIN32
#include <iphlpapi.h>
#include <time.h>
#include <shellapi.h>

#define WPATH 2048 // символов UTF-16 на путь

static void w2u(const wchar_t *w, char *out, int n) {
    out[0] = 0;
    if (!w) return;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL) <= 0) out[0] = 0;
}

static int u2w(const char *s, wchar_t *out, int n) {
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0;
}

static long long ft_unix(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return -1;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static long long file_mtime(const wchar_t *p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return -1;
    return ft_unix(&d.ftLastWriteTime);
}

static int is_dir(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------- порт → PID ----------
// Java на Windows часто слушает двухстековый сокет "::", и тогда порт есть только в таблице IPv6.
// Поэтому смотрим обе таблицы.
typedef struct { DWORD pid; unsigned port; } listener_t;

static int list_listeners(listener_t *out, int max) {
    int n = 0;
    for (int fam = 0; fam < 2; fam++) {
        ULONG size = 0;
        ULONG af = fam ? AF_INET6 : AF_INET;
        GetExtendedTcpTable(NULL, &size, FALSE, af, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (!size) continue;
        size += 4096;
        void *t = malloc(size);
        if (!t) continue;
        if (GetExtendedTcpTable(t, &size, FALSE, af, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
            DWORD cnt = fam ? ((MIB_TCP6TABLE_OWNER_PID *)t)->dwNumEntries : ((MIB_TCPTABLE_OWNER_PID *)t)->dwNumEntries;
            for (DWORD i = 0; i < cnt && n < max; i++) {
                DWORD pid, lp;
                if (fam) { pid = ((MIB_TCP6TABLE_OWNER_PID *)t)->table[i].dwOwningPid; lp = ((MIB_TCP6TABLE_OWNER_PID *)t)->table[i].dwLocalPort; }
                else { pid = ((MIB_TCPTABLE_OWNER_PID *)t)->table[i].dwOwningPid; lp = ((MIB_TCPTABLE_OWNER_PID *)t)->table[i].dwLocalPort; }
                unsigned port = ntohs((u_short)lp);
                int dup = 0;
                for (int k = 0; k < n; k++) if (out[k].pid == pid && out[k].port == port) { dup = 1; break; }
                if (!dup) { out[n].pid = pid; out[n].port = port; n++; }
            }
        }
        free(t);
    }
    return n;
}

static DWORD listener_pid(unsigned port) {
    static listener_t l[2048];
    int n = list_listeners(l, 2048);
    for (int i = 0; i < n; i++) if (l[i].port == port && l[i].pid) return l[i].pid;
    return 0;
}

// ---------- сведения о процессе (из лабы) ----------
static int process_exe(DWORD pid, wchar_t *out, DWORD n) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    DWORD len = n;
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &len);
    CloseHandle(h);
    return ok ? 1 : 0;
}

static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *b = p;
    for (const wchar_t *q = p; *q; q++) if (*q == L'\\' || *q == L'/') b = q + 1;
    return b;
}

static long long process_start(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME c, e, k, u;
    long long t = GetProcessTimes(h, &c, &e, &k, &u) ? ft_unix(&c) : 0;
    CloseHandle(h);
    return t > 0 ? t : 0;
}

typedef LONG(NTAPI *nt_query_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } ustr_t;

static nt_query_fn nt_query(void) {
    static nt_query_fn q = NULL;
    if (!q) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) q = (nt_query_fn)(void *)GetProcAddress(nt, "NtQueryInformationProcess");
    }
    return q;
}

// Строка из параметров процесса (RTL_USER_PROCESS_PARAMETERS в его PEB): рабочая папка или
// командная строка. Нужны QUERY_INFORMATION и VM_READ; процесс того же пользователя их обычно даёт.
// Поддерживает 32-битную Java (WOW64). Возвращает malloc-строку или NULL, *err — короткая причина.
typedef struct { LONG ExitStatus; PVOID PebBaseAddress; ULONG_PTR AffinityMask; LONG BasePriority; ULONG_PTR Pid; ULONG_PTR Parent; } pbi_t;
enum { PEB_CWD, PEB_CMDLINE };

static wchar_t *peb_string(DWORD pid, int which, const char **err) {
    nt_query_fn q = nt_query();
    if (!q) { *err = "ntapi"; return NULL; }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { *err = "open"; return NULL; }
    BOOL wow = FALSE;
    IsWow64Process(h, &wow);
    USHORT len = 0;
    ULONGLONG buf_addr = 0;
    SIZE_T got = 0;
    wchar_t *out = NULL;
    if (!wow) {
        pbi_t pbi;
        ULONG rl = 0;
        if (q(h, 0 /* ProcessBasicInformation */, &pbi, sizeof pbi, &rl) != 0 || !pbi.PebBaseAddress) { *err = "pbi"; goto out; }
        ULONGLONG params = 0;
        if (!ReadProcessMemory(h, (BYTE *)pbi.PebBaseAddress + 0x20, &params, sizeof params, &got) || !params) { *err = "peb"; goto out; }
        struct { USHORT Length, MaximumLength; ULONG pad; ULONGLONG Buffer; } us;
        ULONGLONG off = which == PEB_CWD ? 0x38 : 0x70; // CurrentDirectory.DosPath | CommandLine
        if (!ReadProcessMemory(h, (void *)(ULONG_PTR)(params + off), &us, sizeof us, &got)) { *err = "params"; goto out; }
        len = us.Length;
        buf_addr = us.Buffer;
    } else {
        ULONG_PTR peb32 = 0;
        ULONG rl = 0;
        if (q(h, 26 /* ProcessWow64Information */, &peb32, sizeof peb32, &rl) != 0 || !peb32) { *err = "peb32"; goto out; }
        ULONG params = 0;
        if (!ReadProcessMemory(h, (BYTE *)peb32 + 0x10, &params, sizeof params, &got) || !params) { *err = "peb32"; goto out; }
        struct { USHORT Length, MaximumLength; ULONG Buffer; } us;
        ULONG off = which == PEB_CWD ? 0x24 : 0x40;
        if (!ReadProcessMemory(h, (void *)(ULONG_PTR)(params + off), &us, sizeof us, &got)) { *err = "params32"; goto out; }
        len = us.Length;
        buf_addr = us.Buffer;
    }
    if (len == 0 || !buf_addr) { *err = "empty"; goto out; }
    out = malloc((size_t)len + sizeof(wchar_t));
    if (!out) { *err = "memory"; goto out; }
    if (!ReadProcessMemory(h, (void *)(ULONG_PTR)buf_addr, out, len, &got)) { *err = "read"; free(out); out = NULL; goto out; }
    out[len / sizeof(wchar_t)] = 0;
out:
    CloseHandle(h);
    return out;
}

// Командная строка чужого процесса (malloc) или NULL. Основной способ — NtQueryInformationProcess
// (класс 60, Windows 8.1+), как в лабе; если он не сработал — чтение из PEB.
static wchar_t *process_cmdline(DWORD pid, const char **err) {
    nt_query_fn q = nt_query();
    if (!q) { *err = "ntapi"; return NULL; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { *err = "open"; return NULL; }
    ULONG need = 0, size = 4096;
    void *buf = NULL;
    LONG st = 0;
    for (int tries = 0; tries < 4; tries++) {
        free(buf);
        buf = malloc(size);
        if (!buf) break;
        st = q(h, 60 /* ProcessCommandLineInformation */, buf, size, &need);
        if (st == (LONG)0xC0000004 /* INFO_LENGTH_MISMATCH */ && need > size) { size = need + 64; continue; }
        break;
    }
    CloseHandle(h);
    if (buf && st == 0) {
        ustr_t *us = (ustr_t *)buf;
        size_t n = us->Length / sizeof(wchar_t);
        wchar_t *out = malloc(sizeof(wchar_t) * (n + 1));
        if (out) { memcpy(out, us->Buffer, n * sizeof(wchar_t)); out[n] = 0; }
        free(buf);
        if (out) return out;
    } else {
        free(buf);
    }
    return peb_string(pid, PEB_CMDLINE, err);
}

static int process_cwd(DWORD pid, wchar_t *out, size_t n, const char **err) {
    out[0] = 0;
    wchar_t *s = peb_string(pid, PEB_CWD, err);
    if (!s) return 0;
    int ok = wcslen(s) < n;
    if (ok) wcscpy(out, s);
    else *err = "long";
    free(s);
    return ok;
}

// session.lock: 1 — занят игрой, 0 — свободен, -1 — не проверить. Открываем только на чтение
// с полным совместным доступом и сразу снимаем свою блокировку: файл игры не трогаем.
static int lock_state(const wchar_t *p) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION ? 1 : -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    int st;
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        UnlockFileEx(h, 0, 1, 0, &ov);
        st = 0;
    } else {
        st = GetLastError() == ERROR_LOCK_VIOLATION ? 1 : -1;
    }
    CloseHandle(h);
    return st;
}

static int scan_saves(const wchar_t *saves, rs_world *w, int max) {
    wchar_t pat[WPATH + 4];
    _snwprintf(pat, WPATH + 4, L"%ls\\*", saves);
    pat[WPATH + 3] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t lockp[WPATH + 300];
        _snwprintf(lockp, WPATH + 300, L"%ls\\%ls\\session.lock", saves, fd.cFileName);
        lockp[WPATH + 299] = 0;
        long long mt = file_mtime(lockp);
        if (mt < 0) continue; // без session.lock это не мир
        rs_world *x = &w[n];
        w2u(fd.cFileName, x->name, sizeof x->name);
        if (!x->name[0]) continue;
        x->locked = lock_state(lockp);
        x->lock_mtime = mt;
        n++;
    } while (n < max && FindNextFileW(fh, &fd));
    FindClose(fh);
    return n;
}

static void resolve_error(unsigned port, const char *reason, const char *detail) {
    sbuf l = {0};
    sb_put(&l, "resolve_error");
    sb_field_int(&l, "port", port);
    sb_field(&l, "reason", reason);
    if (detail) sb_field(&l, "detail", detail);
    sb_emit(&l);
}

static void join_world(char *out, size_t n, const char *saves, const char *name) {
    snprintf(out, n, "%s\\%s", saves, name);
}

static void resolve_pid(DWORD pid, unsigned port) {
    static wchar_t wexe[WPATH], wcwd[WPATH], wsaves[WPATH];
    static char exe[RS_PATH], cwd[RS_PATH], gamedir[RS_PATH], saves[RS_PATH + 8], path[RS_PATH + 300];
    static rs_world worlds[MAX_WORLDS];
    char diag[96] = "";

    exe[0] = 0;
    if (process_exe(pid, wexe, WPATH)) w2u(base_name(wexe), exe, sizeof exe);

    // командная строка → аргументы UTF-8; наружу уходит только --gameDir и маркер лаунчера
    rs_args a = { NULL, NULL, "unknown" };
    const char *err = NULL;
    wchar_t *cl = process_cmdline(pid, &err);
    int wargc = 0;
    LPWSTR *wargv = cl ? CommandLineToArgvW(cl, &wargc) : NULL;
    char **uargv = wargv && wargc > 0 ? calloc((size_t)wargc, sizeof(char *)) : NULL;
    if (uargv) {
        for (int i = 0; i < wargc; i++) {
            int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            uargv[i] = need > 0 ? malloc((size_t)need) : NULL;
            if (uargv[i]) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, uargv[i], need, NULL, NULL);
        }
        rs_parse_args(wargc, uargv, &a);
    } else {
        snprintf(diag + strlen(diag), sizeof diag - strlen(diag), "%scmdline:%s", diag[0] ? "," : "", cl ? "split" : err);
    }

    const char *cerr = NULL;
    cwd[0] = 0;
    if (process_cwd(pid, wcwd, WPATH, &cerr)) w2u(wcwd, cwd, sizeof cwd);
    else snprintf(diag + strlen(diag), sizeof diag - strlen(diag), "%scwd:%s", diag[0] ? "," : "", cerr);

    const char *how = "cwd";
    int ok = rs_game_dir(&a, cwd[0] ? cwd : NULL, gamedir, sizeof gamedir, &how);
    // a.gamedir указывает внутрь uargv: освобождаем только после rs_game_dir
    if (uargv) { for (int i = 0; i < wargc; i++) free(uargv[i]); free(uargv); }
    if (wargv) LocalFree(wargv);
    free(cl);
    if (!ok) { resolve_error(port, "no_gamedir", diag[0] ? diag : how); return; }

    snprintf(saves, sizeof saves, "%s\\saves", gamedir);
    if (!u2w(saves, wsaves, WPATH) || !is_dir(wsaves)) {
        sbuf l = {0};
        sb_put(&l, "resolve_error");
        sb_field_int(&l, "port", port);
        sb_field(&l, "reason", "no_saves");
        sb_field(&l, "gamedir", gamedir);
        sb_field(&l, "source", how);
        sb_field(&l, "launcher", a.launcher);
        if (diag[0]) sb_field(&l, "diag", diag);
        sb_emit(&l);
        return;
    }

    int n = scan_saves(wsaves, worlds, MAX_WORLDS);
    long long start = process_start(pid);
    if (!start) snprintf(diag + strlen(diag), sizeof diag - strlen(diag), "%sstart:open", diag[0] ? "," : "");
    int picked, cand[MAX_CAND], ncand;
    int method = rs_pick(worlds, n, start, &picked, cand, MAX_CAND, &ncand);
    long long now = (long long)time(NULL);

    if (method == RS_ASK) {
        for (int i = 0; i < ncand; i++) {
            rs_world *x = &worlds[cand[i]];
            join_world(path, sizeof path, saves, x->name);
            sbuf l = {0};
            sb_put(&l, "candidate");
            sb_field_int(&l, "port", port);
            sb_field(&l, "world", path);
            sb_field(&l, "name", x->name);
            sb_field(&l, "locked", x->locked == 1 ? "1" : x->locked == 0 ? "0" : "?");
            sb_field_int(&l, "lock_age_s", x->lock_mtime >= 0 && now >= x->lock_mtime ? now - x->lock_mtime : -1);
            sb_emit(&l);
        }
    }
    sbuf l = {0};
    sb_put(&l, "world");
    sb_field_int(&l, "port", port);
    sb_field_int(&l, "pid", pid);
    sb_field(&l, "exe", exe);
    sb_field(&l, "launcher", a.launcher);
    sb_field(&l, "source", how);
    sb_field(&l, "gamedir", gamedir);
    sb_field(&l, "saves", saves);
    sb_field(&l, "method", method == RS_LOCK ? "lock" : method == RS_MTIME ? "mtime" : "ask");
    if (picked >= 0) {
        join_world(path, sizeof path, saves, worlds[picked].name);
        sb_field(&l, "world", path);
        sb_field(&l, "name", worlds[picked].name);
    } else {
        sb_field(&l, "world", "");
        sb_field(&l, "name", "");
    }
    sb_field_int(&l, "worlds", n);
    sb_field_int(&l, "candidates", ncand);
    if (diag[0]) sb_field(&l, "diag", diag);
    sb_emit(&l);
}

static void resolve_port(unsigned port) {
    DWORD pid = listener_pid(port);
    if (!pid) { resolve_error(port, "no_listener", NULL); return; }
    resolve_pid(pid, port);
}

// CLI: по PID процесса игры, без порта (диагностика: PID виден в Диспетчере задач)
void cmd_resolve_pid(const char *pid_s) {
    long pid = pid_s ? atol(pid_s) : 0;
    if (pid <= 0) { resolve_error(0, "bad_pid", pid_s ? pid_s : ""); return; }
    resolve_pid((DWORD)pid, 0);
}

void cmd_resolve(const char *port_s) {
    int port = port_s ? atoi(port_s) : 0;
    if (port < 1 || port > 65535) { resolve_error(0, "bad_port", port_s ? port_s : ""); return; }
    resolve_port((unsigned)port);
}

void cmd_resolve_all(void) {
    static listener_t l[2048];
    int n = list_listeners(l, 2048), count = 0;
    for (int i = 0; i < n; i++) {
        wchar_t exe[WPATH];
        if (!l[i].pid || !process_exe(l[i].pid, exe, WPATH)) continue;
        const wchar_t *bn = base_name(exe);
        if (_wcsicmp(bn, L"java.exe") && _wcsicmp(bn, L"javaw.exe")) continue;
        resolve_port(l[i].port);
        count++;
    }
    char d[48];
    snprintf(d, sizeof d, "resolve_all_done\tcount=%d", count);
    out_line(d);
}

#else
// ======================================================================
// не Windows: только для тестов протокола
// ======================================================================
void cmd_resolve(const char *port_s) {
    (void)port_s;
    emit_error("unsupported", "resolve");
}
void cmd_resolve_all(void) { emit_error("unsupported", "resolve"); }
void cmd_resolve_pid(const char *pid_s) {
    (void)pid_s;
    emit_error("unsupported", "resolve");
}
#endif
