#define FD_SETSIZE 256
#include <winsock2.h>
#include "run.h"
#include <string.h>

static intptr_t socket_error(void) {
    switch (WSAGetLastError()) {
        case WSAEBADF: case WSAENOTSOCK: return -9;
        case WSAEACCES: return -13;
        case WSAEFAULT: return -14;
        case WSAEINVAL: return -22;
        case WSAEMFILE: return -24;
        case WSAEWOULDBLOCK: return -11;
        case WSAEINPROGRESS: return -115;
        case WSAEALREADY: return -114;
        case WSAEADDRINUSE: return -98;
        case WSAEADDRNOTAVAIL: return -99;
        case WSAEAFNOSUPPORT: return -97;
        case WSAECONNRESET: return -104;
        case WSAECONNREFUSED: return -111;
        case WSAETIMEDOUT: return -110;
        case WSAENETUNREACH: return -101;
        case WSAEHOSTUNREACH: return -113;
        case WSAEMSGSIZE: return -90;
        case WSAENOTCONN: return -107;
        default: return -5;
    }
}

static intptr_t add_socket(NrRun *run, SOCKET s, int nonblock) {
    unsigned fd;
    for (fd = 0; fd < 256 && run->file_access[fd]; ++fd) {}
    if (fd == 256) { closesocket(s); return -24; }
    if (nonblock) {
        u_long mode = 1;
        if (ioctlsocket(s, FIONBIO, &mode)) { intptr_t error = socket_error(); closesocket(s); return error; }
    }
    run->sockets[fd] = (uintptr_t)s;
    run->file_access[fd] = 4;
    run->nonblock[fd] = nonblock != 0;
    return fd;
}

static intptr_t select_sockets(NrRun *run, const uintptr_t args[6]) {
    if (args[0] > 256) return -22;
    size_t bytes = ((args[0] + sizeof(uintptr_t) * 8 - 1) / (sizeof(uintptr_t) * 8)) * sizeof(uintptr_t);
    unsigned char bits[3][32] = {{0}};
    fd_set sets[3];
    for (unsigned i = 0; i < 3; ++i) {
        FD_ZERO(&sets[i]);
        if (!args[i + 1]) continue;
        if (!nr_memory(args[i + 1], bytes, 1)) return -14;
        memcpy(bits[i], (const void *)args[i + 1], bytes);
        for (unsigned fd = 0; fd < args[0]; ++fd) {
            if (!(bits[i][fd / 8] & (1u << (fd % 8)))) continue;
            if (run->file_access[fd] != 4) return -9;
            FD_SET((SOCKET)run->sockets[fd], &sets[i]);
        }
    }
    struct timeval timeout, *wait = NULL;
    ULONGLONG start = GetTickCount64();
    if (args[4]) {
        if (!nr_memory(args[4], 2 * sizeof(uintptr_t), 1)) return -14;
        intptr_t input[2]; memcpy(input, (const void *)args[4], sizeof(input));
        if (input[0] < 0 || input[0] > LONG_MAX || input[1] < 0 || input[1] >= 1000000) return -22;
        timeout.tv_sec = (long)input[0]; timeout.tv_usec = (long)input[1]; wait = &timeout;
    }
    int count = select(0, &sets[0], &sets[1], &sets[2], wait);
    if (count == SOCKET_ERROR) return socket_error();
    for (unsigned i = 0; i < 3; ++i) {
        if (!args[i + 1]) continue;
        memset(bits[i], 0, bytes);
        for (unsigned fd = 0; fd < args[0]; ++fd)
            if (run->file_access[fd] == 4 && FD_ISSET((SOCKET)run->sockets[fd], &sets[i])) bits[i][fd / 8] |= (unsigned char)(1u << (fd % 8));
        memcpy((void *)args[i + 1], bits[i], bytes);
    }
    if (args[4]) {
        uint64_t microseconds = (uint64_t)timeout.tv_sec * 1000000 + timeout.tv_usec;
        uint64_t used = (GetTickCount64() - start) * 1000;
        if (used >= microseconds) microseconds = 0; else microseconds -= used;
        uintptr_t output[] = {(uintptr_t)(microseconds / 1000000), (uintptr_t)(microseconds % 1000000)};
        memcpy((void *)args[4], output, sizeof(output));
    }
    return count;
}

int nr_net(NrRun *run, uintptr_t number, const uintptr_t input[6], intptr_t *result) {
#ifdef _WIN64
    enum {CLOSE = 3, IOCTL = 16, FCNTL = 72, SELECT = 23};
#else
    enum {CLOSE = 6, IOCTL = 54, FCNTL = 55, SELECT = 142};
#endif
    if (number == SELECT) { *result = select_sockets(run, input); return 1; }
    uintptr_t args[6]; memcpy(args, input, sizeof(args));
    int operation = 0;
#ifdef _WIN64
    switch (number) {
        case 41: operation = 1; break; case 49: operation = 2; break; case 42: operation = 3; break;
        case 50: operation = 4; break; case 43: operation = 5; break; case 51: operation = 6; break;
        case 44: operation = 11; break; case 45: operation = 12; break; case 48: operation = 13; break;
        case 54: operation = 14; break; default: break;
    }
#else
    if (number == 102) {
        static const unsigned char counts[] = {0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5};
        operation = (int)input[0];
        if (operation < 1 || operation > 15) { *result = -38; return 1; }
        if (!nr_memory(input[1], counts[operation] * sizeof(uintptr_t), 0)) { *result = -14; return 1; }
        memset(args, 0, sizeof(args));
        memcpy(args, (const void *)input[1], counts[operation] * sizeof(uintptr_t));
    }
#endif
    uintptr_t fd = args[0];
    if (!operation && ((number != CLOSE && number != IOCTL && number != FCNTL) || fd >= 256 || run->file_access[fd] != 4)) return 0;
    if (operation == 1) {
        if (args[0] != AF_INET || ((args[1] & 15) != SOCK_DGRAM && (args[1] & 15) != SOCK_STREAM) ||
            (args[1] & ~(uintptr_t)(15 | 0x800 | 0x80000))) { *result = -38; return 1; }
        static int initialized;
        if (!initialized) {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data)) { *result = -5; return 1; }
            initialized = 1;
        }
        SOCKET s = socket((int)args[0], (int)(args[1] & 15), (int)args[2]);
        *result = s == INVALID_SOCKET ? socket_error() : add_socket(run, s, (args[1] & 0x800) != 0);
        return 1;
    }
    if (fd >= 256 || run->file_access[fd] != 4) { *result = -9; return 1; }
    SOCKET s = (SOCKET)run->sockets[fd];
    if (number == CLOSE && !operation) {
        *result = closesocket(s) == 0 ? 0 : socket_error();
        if (!*result) { run->file_access[fd] = 0; run->sockets[fd] = 0; run->nonblock[fd] = 0; }
        return 1;
    }
    if (number == FCNTL && !operation) {
        if (args[1] == 3) *result = 2 | (run->nonblock[fd] ? 0x800 : 0);
        else if (args[1] == 4 && !(args[2] & ~(uintptr_t)(2 | 0x800))) {
            u_long mode = (args[2] & 0x800) != 0;
            *result = ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : socket_error();
            if (!*result) run->nonblock[fd] = mode != 0;
        } else *result = -38;
        return 1;
    }
    if (number == IOCTL && !operation) {
        if (args[1] != 0x5421 || !nr_memory(args[2], 4, 0)) { *result = args[1] == 0x5421 ? -14 : -38; return 1; }
        u_long mode; memcpy(&mode, (const void *)args[2], 4); mode = mode != 0;
        *result = ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : socket_error();
        if (!*result) run->nonblock[fd] = mode != 0;
        return 1;
    }
    if (operation == 2 || operation == 3) {
        if (args[2] != 16 || !nr_memory(args[1], 16, 0)) { *result = args[2] != 16 ? -22 : -14; return 1; }
        struct sockaddr_in address; memcpy(&address, (const void *)args[1], 16);
        if (address.sin_family != AF_INET) { *result = -97; return 1; }
        int r = operation == 2 ? bind(s, (const struct sockaddr *)&address, 16) : connect(s, (const struct sockaddr *)&address, 16);
        *result = r == 0 ? 0 : socket_error(); return 1;
    }
    if (operation == 4) { *result = listen(s, (int)args[1]) == 0 ? 0 : socket_error(); return 1; }
    if (operation == 13) { *result = shutdown(s, (int)args[1]) == 0 ? 0 : socket_error(); return 1; }
    if (operation == 14) {
        if (args[1] != 1 || args[4] != 4 || !nr_memory(args[3], 4, 0)) { *result = -38; return 1; }
        int option;
        switch (args[2]) { case 2: option = SO_REUSEADDR; break; case 6: option = SO_BROADCAST; break;
            case 7: option = SO_SNDBUF; break; case 8: option = SO_RCVBUF; break; default: *result = -38; return 1; }
        int value; memcpy(&value, (const void *)args[3], 4);
        *result = setsockopt(s, SOL_SOCKET, option, (const char *)&value, 4) == 0 ? 0 : socket_error(); return 1;
    }
    if (operation == 5 || operation == 6) {
        struct sockaddr_in address; int bytes = sizeof(address);
        if (args[1] && (!nr_memory(args[2], 4, 1) || !nr_memory(args[1], 16, 1))) { *result = -14; return 1; }
        int capacity = 0; if (args[1]) memcpy(&capacity, (const void *)args[2], 4);
        SOCKET accepted = INVALID_SOCKET;
        int r;
        if (operation == 5) { accepted = accept(s, (struct sockaddr *)&address, &bytes); r = accepted == INVALID_SOCKET ? SOCKET_ERROR : 0; }
        else r = getsockname(s, (struct sockaddr *)&address, &bytes);
        if (r == SOCKET_ERROR) { *result = socket_error(); return 1; }
        if (args[1]) {
            if (capacity > bytes) capacity = bytes;
            if (capacity > 0) memcpy((void *)args[1], &address, (size_t)capacity);
            memcpy((void *)args[2], &bytes, 4);
        }
        *result = operation == 5 ? add_socket(run, accepted, 0) : 0; return 1;
    }
    if (operation == 9 || operation == 10 || operation == 11 || operation == 12) {
        int receiving = operation == 10 || operation == 12;
        if (args[2] > INT_MAX || args[3] || !nr_memory(args[1], args[2], receiving)) {
            *result = args[3] || args[2] > INT_MAX ? -38 : -14; return 1;
        }
        struct sockaddr_in address; int bytes = 16, capacity = 0;
        if (args[4]) {
            if (receiving) {
                if (!nr_memory(args[5], 4, 1) || !nr_memory(args[4], 16, 1)) { *result = -14; return 1; }
                memcpy(&capacity, (const void *)args[5], 4);
            } else {
                if (args[5] != 16 || !nr_memory(args[4], 16, 0)) { *result = -22; return 1; }
                memcpy(&address, (const void *)args[4], 16);
                if (address.sin_family != AF_INET) { *result = -97; return 1; }
            }
        }
        int r = receiving ? recvfrom(s, (char *)args[1], (int)args[2], 0, args[4] ? (struct sockaddr *)&address : NULL, args[4] ? &bytes : NULL) :
                            sendto(s, (const char *)args[1], (int)args[2], 0, args[4] ? (const struct sockaddr *)&address : NULL, args[4] ? 16 : 0);
        if (r == SOCKET_ERROR) { *result = socket_error(); return 1; }
        if (receiving && args[4]) {
            if (capacity > bytes) capacity = bytes;
            if (capacity > 0) memcpy((void *)args[4], &address, (size_t)capacity);
            memcpy((void *)args[5], &bytes, 4);
        }
        *result = r; return 1;
    }
    *result = -38;
    return 1;
}
