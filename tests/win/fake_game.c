// Поддельная «игра» для проверки resolve под Wine: слушает TCP-порт и держит session.lock одного
// мира заблокированным, как Minecraft. Аргументы как у Java: всё после имени программы попадает
// в командную строку процесса, хелпер разбирает её сам.
//   fake_game.exe <port> <lock: путь к session.lock или "-"> [любые аргументы, например --gameDir X]
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)atoi(argv[1]));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 1)) { printf("bind failed\n"); return 3; }
    HANDLE lock = INVALID_HANDLE_VALUE;
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc); // путь с кириллицей нужен в UTF-16
    if (strcmp(argv[2], "-") && wargv && wargc > 2) {
        lock = CreateFileW(wargv[2], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        OVERLAPPED ov = { 0 };
        if (lock == INVALID_HANDLE_VALUE || !LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) { printf("lock failed %lu\n", GetLastError()); return 4; }
    }
    printf("ready pid=%lu\n", GetCurrentProcessId());
    fflush(stdout);
    Sleep(60000);
    return 0;
}
