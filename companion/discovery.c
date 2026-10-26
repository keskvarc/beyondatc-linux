#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef PTT_KEY
#define PTT_KEY VK_DOWN
#endif

static int ptt_down;

static int set_ptt(int down) {
    if (down == ptt_down) return 1;
    INPUT event = {0};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = PTT_KEY;
    event.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (down ? 0 : KEYEVENTF_KEYUP);
    if (SendInput(1, &event, sizeof(event)) != 1) {
        fprintf(stderr, "PTT bridge: SendInput failed: %lu\n", GetLastError());
        return 0;
    }
    ptt_down = down;
    fprintf(stderr, "PTT bridge: key=%u %s, observed=%d\n",
            (unsigned)PTT_KEY, down ? "down" : "released",
            !!(GetAsyncKeyState(PTT_KEY) & 0x8000));
    fflush(stderr);
    return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    (void)instance; (void)previous; (void)show;
    char* end;
    unsigned long port = strtoul(command, &end, 10);
    if (!port || port > 65535 || *end) return 2;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 3;
    SOCKET socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == INVALID_SOCKET) return 4;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, (struct sockaddr*)&address, sizeof(address))) return 5;
    DWORD timeout = 1000;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout))) return 6;
    int result = 0;
    unsigned char state;
    for (;;) {
        int length = recv(socket_fd, (char*)&state, 1, 0);
        if (!length) break;
        if (length == SOCKET_ERROR) {
            fprintf(stderr, "PTT bridge: supervisor connection failed: %d\n", WSAGetLastError());
            result = 7;
            break;
        }
        if (state > 1 || !set_ptt(state)) {
            fprintf(stderr, "PTT bridge: invalid state or input delivery failure\n");
            result = 8;
            break;
        }
    }
    if (!set_ptt(0)) result = 9;
    closesocket(socket_fd);
    WSACleanup();
    return result;
}
