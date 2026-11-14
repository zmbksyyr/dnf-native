#define FD_SETSIZE 256
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "run.h"
#include <winternl.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

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
        case WSAENOPROTOOPT: return -92;
        case WSAENOTCONN: return -107;
        default: return -5;
    }
}

static intptr_t socket_io_error(void) {
    return WSAGetLastError() == WSAETIMEDOUT ? -11 : socket_error();
}

static intptr_t wait_socket(SOCKET socket, int writing, uint64_t timeout) {
    if (!timeout) return 0;
    if (timeout / 1000000 > LONG_MAX) return -22;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    struct timeval duration = {(long)(timeout / 1000000), (long)(timeout % 1000000)};
    int ready = select(0, writing ? NULL : &set, writing ? &set : NULL, NULL, &duration);
    if (ready == SOCKET_ERROR) return socket_error();
    return ready ? 0 : -11;
}

static int interface_mac(unsigned char mac[6]) {
    ULONG bytes = 0;
    if (GetAdaptersInfo(NULL, &bytes) != ERROR_BUFFER_OVERFLOW || !bytes) return 0;
    PIP_ADAPTER_INFO adapters = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!adapters) return 0;
    int found = 0;
    if (GetAdaptersInfo(adapters, &bytes) == ERROR_SUCCESS) {
        for (PIP_ADAPTER_INFO adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->AddressLength != 6) continue;
            memcpy(mac, adapter->Address, 6);
            found = 1;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, adapters);
    return found;
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
    SOCKET sockets[256] = {0};
    fd_set sets[3];
    for (unsigned i = 0; i < 3; ++i) {
        FD_ZERO(&sets[i]);
        if (!args[i + 1]) continue;
        if (!nr_memory(args[i + 1], bytes, 1)) return -14;
        memcpy(bits[i], (const void *)args[i + 1], bytes);
        for (unsigned fd = 0; fd < args[0]; ++fd) {
            if (!(bits[i][fd / 8] & (1u << (fd % 8)))) continue;
            if (run->file_access[fd] != 4) return -9;
            sockets[fd] = (SOCKET)run->sockets[fd];
            if (sockets[fd] == INVALID_SOCKET) return -9;
            FD_SET(sockets[fd], &sets[i]);
        }
    }
    struct timeval timeout, *wait = NULL;
    ULONGLONG start = GetTickCount64();
    if (args[4]) {
        if (!nr_memory(args[4], 2 * sizeof(uintptr_t), 1)) return -14;
        intptr_t input[2]; memcpy(input, (const void *)args[4], sizeof(input));
        if (input[0] < 0 || input[1] < 0) return -22;
        /* Linux select carries excess tv_usec into seconds. Original bridge
           uses this form for its ten-second main-loop sleep. */
        uint64_t seconds = (uint64_t)input[0] + (uint64_t)input[1] / 1000000;
        if (seconds > LONG_MAX) return -22;
        timeout.tv_sec = (long)seconds; timeout.tv_usec = (long)(input[1] % 1000000); wait = &timeout;
    }
    int count;
    if (!sets[0].fd_count && !sets[1].fd_count && !sets[2].fd_count) {
        if (!wait) Sleep(INFINITE);
        else {
            uint64_t milliseconds = (uint64_t)timeout.tv_sec * 1000 + (timeout.tv_usec + 999) / 1000;
            while (milliseconds) {
                DWORD part = milliseconds > MAXDWORD - 1 ? MAXDWORD - 1 : (DWORD)milliseconds;
                Sleep(part); milliseconds -= part;
            }
        }
        count = 0;
    } else count = select(0, &sets[0], &sets[1], &sets[2], wait);
    if (count == SOCKET_ERROR) return socket_error();
    /* Winsock can report readiness when another thread closes a socket.
       Reject that stale result before returning guest descriptor sets. */
    for (unsigned fd = 0; fd < args[0]; ++fd)
        if (((bits[0][fd / 8] | bits[1][fd / 8] | bits[2][fd / 8]) & (1u << (fd % 8))) &&
            (run->file_access[fd] != 4 || (SOCKET)run->sockets[fd] != sockets[fd])) return -9;
    count = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!args[i + 1]) continue;
        unsigned char output[32] = {0};
        for (unsigned fd = 0; fd < args[0]; ++fd) {
            unsigned char bit = (unsigned char)(1u << (fd % 8));
            if (!(bits[i][fd / 8] & bit)) continue;
            if (FD_ISSET(sockets[fd], &sets[i])) { output[fd / 8] |= bit; ++count; }
        }
        memcpy((void *)args[i + 1], output, bytes);
    }
    if (args[4]) {
        uint64_t microseconds = (uint64_t)timeout.tv_sec * 1000000 + timeout.tv_usec;
        uint64_t used = (GetTickCount64() - start) * 1000;
        if (!count || used >= microseconds) microseconds = 0; else microseconds -= used;
        uintptr_t output[] = {(uintptr_t)(microseconds / 1000000), (uintptr_t)(microseconds % 1000000)};
        memcpy((void *)args[4], output, sizeof(output));
    }
    return count;
}

static intptr_t pipe_events(NrRun *run, unsigned fd, short requested) {
    typedef NTSTATUS (NTAPI *QueryFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    QueryFile query = (QueryFile)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    if (!query) return -38;
    IO_STATUS_BLOCK io;
    FILE_PIPE_LOCAL_INFORMATION info;
    if (query(run->files[fd], &io, &info, sizeof(info), FilePipeLocalInformation) < 0) return -5;
    short events = 0;
    if (run->file_access[fd] & 1) {
        if (info.ReadDataAvailable) events |= requested & (1 | 64);
        if (info.NamedPipeState == 4) events |= 16; /* FILE_PIPE_CLOSING_STATE */
    }
    if (run->file_access[fd] & 2) {
        if (info.WriteQuotaAvailable >= 4096) events |= requested & (4 | 256);
        if (info.NamedPipeState == 4) events |= 8;
    }
    return events;
}

typedef struct { int32_t fd; int16_t events, revents; } NrPoll;

static intptr_t poll_descriptors(NrRun *run, NrPoll *entries, unsigned length, int timeout) {
    ULONGLONG start = GetTickCount64();
    int ready;
    for (;;) {
        WSAPOLLFD polls[256]; unsigned indices[256]; ULONG count = 0;
        int has_pipes = 0;
        ready = 0;
        for (unsigned i = 0; i < length; ++i) {
            entries[i].revents = 0;
            int fd = entries[i].fd;
            if (fd < 0) continue;
            if (fd >= 256 || !run->file_access[fd]) { entries[i].revents = 32; ++ready; continue; }
            if (entries[i].events & ~(1 | 2 | 4 | 64 | 256)) return -38;
            if (run->file_access[fd] != 4) {
                short events = 0;
                if (run->files[fd] && GetFileType(run->files[fd]) == FILE_TYPE_PIPE) {
                    intptr_t state = pipe_events(run, (unsigned)fd, entries[i].events);
                    if (state < 0) return state;
                    events = (short)state;
                    has_pipes = 1;
                } else {
                    /* Regular files are immediately ready on Linux. */
                    if (run->file_access[fd] & 1) events |= entries[i].events & (1 | 64);
                    if (run->file_access[fd] & 2) events |= entries[i].events & (4 | 256);
                }
                entries[i].revents = events;
                if (events) ++ready;
                continue;
            }
            polls[count].fd = (SOCKET)run->sockets[fd];
            polls[count].events = 0; polls[count].revents = 0;
            if (entries[i].events & (1 | 64)) polls[count].events |= POLLRDNORM;
            if (entries[i].events & 2) polls[count].events |= POLLPRI;
            if (entries[i].events & (4 | 256)) polls[count].events |= POLLWRNORM;
            indices[count++] = i;
        }
        ULONGLONG elapsed = GetTickCount64() - start;
        int expired = timeout >= 0 && elapsed >= (unsigned)timeout;
        int milliseconds = ready || expired ? 0 : timeout < 0 ? -1 : timeout - (int)elapsed;
        /* Anonymous pipe handles have no waitable readiness event. Bound the
           sleep/socket wait so both kinds of descriptors are checked. */
        if (has_pipes && (milliseconds < 0 || milliseconds > 10)) milliseconds = 10;
        if (!count) Sleep(milliseconds < 0 ? INFINITE : (DWORD)milliseconds);
        else if (WSAPoll(polls, count, milliseconds) == SOCKET_ERROR) return socket_error();
        for (ULONG i = 0; i < count; ++i) {
            unsigned at = indices[i];
            short events = 0;
            if (polls[i].revents & POLLRDNORM) events |= entries[at].events & (1 | 64);
            if (polls[i].revents & POLLPRI) events |= entries[at].events & 2;
            if (polls[i].revents & POLLWRNORM) events |= entries[at].events & (4 | 256);
            if (polls[i].revents & POLLERR) events |= 8;
            if (polls[i].revents & POLLHUP) events |= 16;
            if (polls[i].revents & POLLNVAL) events |= 32;
            entries[at].revents = events;
            if (events) ++ready;
        }
        if (ready || expired) break;
    }
    return ready;
}

static intptr_t poll_sockets(NrRun *run, const uintptr_t args[6]) {
    NrPoll entries[256];
    if (args[1] > 256) return -22;
    size_t bytes = (size_t)args[1] * sizeof(entries[0]);
    if (!nr_memory(args[0], bytes, 1)) return -14;
    memcpy(entries, (const void *)args[0], bytes);
    intptr_t ready = poll_descriptors(run, entries, (unsigned)args[1], (int)args[2]);
    if (ready >= 0) memcpy((void *)args[0], entries, bytes);
    return ready;
}

static intptr_t epoll_wait(NrRun *run, NrWatch *watches, uintptr_t address, uintptr_t maximum, intptr_t milliseconds) {
    if (!maximum || maximum > INT_MAX || milliseconds < -1) return -22;
    size_t capacity = maximum > 256 ? 256 : maximum;
    if (!nr_memory(address, capacity * 12, 1)) return -14;
    NrPoll polls[256]; unsigned count = 0;
    for (unsigned fd = 0; fd < 256; ++fd) {
        if (!watches[fd].used) continue;
        polls[count].fd = (int32_t)fd;
        polls[count].events = (int16_t)(watches[fd].events & 5);
        polls[count++].revents = 0;
    }
    intptr_t ready = poll_descriptors(run, polls, count, milliseconds > INT_MAX ? INT_MAX : (int)milliseconds);
    if (ready < 0) return ready;
    intptr_t written = 0;
    for (unsigned i = 0; i < count && (size_t)written < capacity; ++i) {
        uint32_t events = (uint32_t)polls[i].revents & 29;
        if (polls[i].revents & 32) events |= 16;
        if (!events) continue;
        unsigned char *out = (unsigned char *)address + written * 12;
        memcpy(out, &events, 4); memcpy(out + 4, &watches[polls[i].fd].data, 8); ++written;
    }
    return written;
}

int nr_net(NrRun *run, uintptr_t number, const uintptr_t input[6], intptr_t *result) {
#ifdef _WIN64
    enum {READ = 0, WRITE = 1, CLOSE = 3, IOCTL = 16, FCNTL = 72, SELECT = 23, POLL = 7, EP_CREATE = 213, EP_CREATE1 = 291, EP_CTL = 233, EP_WAIT = 232};
#else
    enum {READ = 3, WRITE = 4, CLOSE = 6, IOCTL = 54, FCNTL = 55, SELECT = 142, POLL = 168, EP_CREATE = 254, EP_CREATE1 = 329, EP_CTL = 255, EP_WAIT = 256};
#endif
#ifndef _WIN64
    if (number == 221) number = FCNTL;
#endif
    if (number == SELECT) { *result = select_sockets(run, input); return 1; }
    if (number == POLL) { *result = poll_sockets(run, input); return 1; }
    if (number == EP_CREATE || number == EP_CREATE1) {
        if ((number == EP_CREATE && (intptr_t)input[0] <= 0) || (number == EP_CREATE1 && (input[0] & ~0x80000u))) { *result = -22; return 1; }
        unsigned fd;
        for (fd = 0; fd < 256 && run->file_access[fd]; ++fd) {}
        if (fd == 256) { *result = -24; return 1; }
        run->epolls[fd] = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256 * sizeof(NrWatch));
        if (!run->epolls[fd]) { *result = -12; return 1; }
        run->file_access[fd] = 8;
        *result = fd; return 1;
    }
    if (number == CLOSE && input[0] < 256) {
        unsigned fd = (unsigned)input[0];
        if (run->file_access[fd] == 8) {
            HeapFree(GetProcessHeap(), 0, run->epolls[fd]); run->epolls[fd] = NULL; run->file_access[fd] = 0;
            *result = 0; return 1;
        }
        for (unsigned i = 0; i < 256; ++i) if (run->epolls[i]) run->epolls[i][fd].used = 0;
    }
    if (number == EP_CTL || number == EP_WAIT) {
        if (input[0] >= 256 || run->file_access[input[0]] != 8) { *result = -9; return 1; }
        NrWatch *watches = run->epolls[input[0]];
        if (number == EP_WAIT) { *result = epoll_wait(run, watches, input[1], input[2], (intptr_t)input[3]); return 1; }
        if (input[2] >= 256 || !run->file_access[input[2]]) { *result = -9; return 1; }
        if (run->file_access[input[2]] != 4 &&
            (!run->files[input[2]] || GetFileType(run->files[input[2]]) != FILE_TYPE_PIPE)) {
            *result = -1; return 1;
        }
        NrWatch *watch = &watches[input[2]];
        if (input[1] == 2) { *result = watch->used ? 0 : -2; watch->used = 0; return 1; }
        if (input[1] != 1 && input[1] != 3) { *result = -22; return 1; }
        if (!nr_memory(input[3], 12, 0)) { *result = -14; return 1; }
        uint32_t events; memcpy(&events, (const void *)input[3], 4);
        if (events & ~29u) {
            fprintf(stderr, "epoll unsupported events=0x%x\n", events); *result = -38; return 1;
        }
        if ((input[1] == 1 && watch->used) || (input[1] == 3 && !watch->used)) { *result = watch->used ? -17 : -2; return 1; }
        watch->used = 1; watch->events = events; memcpy(&watch->data, (const void *)(input[3] + 4), 8);
        *result = 0; return 1;
    }
    uintptr_t args[6]; memcpy(args, input, sizeof(args));
    int operation = 0;
#ifdef _WIN64
    switch (number) {
        case 41: operation = 1; break; case 49: operation = 2; break; case 42: operation = 3; break;
        case 50: operation = 4; break; case 43: operation = 5; break; case 51: operation = 6; break;
        case 44: operation = 11; break; case 45: operation = 12; break; case 48: operation = 13; break;
        case 54: operation = 14; break; case 55: operation = 15; break; default: break;
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
    if (operation && GetEnvironmentVariableW(L"DNF_NATIVE_TRACE", NULL, 0))
        fprintf(stderr, "socket op=%d args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx\n", operation,
                (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2],
                (unsigned long long)args[3], (unsigned long long)args[4], (unsigned long long)args[5]);
    if (!operation && ((number != CLOSE && number != IOCTL && number != FCNTL && number != READ && number != WRITE) || fd >= 256 || run->file_access[fd] != 4)) return 0;
    if (operation == 1) {
        if (args[0] == 1) { *result = -97; return 1; } /* AF_UNIX is outside the Windows socket subset. */
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
    if (!operation && (number == READ || number == WRITE)) {
        int receiving = number == READ;
        size_t bytes = args[2] > 0x7ffff000 ? 0x7ffff000 : args[2];
        if (!nr_memory(args[1], bytes, receiving)) { *result = -14; return 1; }
        if (!bytes) { *result = 0; return 1; }
        if (!run->nonblock[fd] &&
            (receiving ? run->receive_timeout[fd] : run->send_timeout[fd]) > (uint64_t)(MAXDWORD - 1) * 1000) {
            intptr_t waited = wait_socket(s, !receiving,
                                          receiving ? run->receive_timeout[fd] : run->send_timeout[fd]);
            if (waited) { *result = waited; return 1; }
        }
        int r = receiving ? recv(s, (char *)args[1], (int)bytes, 0) : send(s, (const char *)args[1], (int)bytes, 0);
        *result = r == SOCKET_ERROR ? socket_io_error() : r; return 1;
    }
    if (number == CLOSE && !operation) {
        /* Publish the closing state before closesocket wakes a blocked select.
           Keep the slot occupied until its remaining state has been cleared. */
        InterlockedExchangePointer((PVOID volatile *)&run->sockets[fd], (PVOID)INVALID_SOCKET);
        *result = closesocket(s) == 0 ? 0 : socket_error();
        if (!*result) {
            run->nonblock[fd] = 0;
            run->receive_timeout[fd] = run->send_timeout[fd] = 0;
            run->file_access[fd] = 0;
        } else run->sockets[fd] = (uintptr_t)s;
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
        if (args[1] == 0x8927) {
            if (!nr_memory(args[2], 32, 1)) { *result = -14; return 1; }
            unsigned char mac[6];
            if (!interface_mac(mac)) { *result = -19; return 1; }
            unsigned char *request = (unsigned char *)args[2];
            memset(request + 16, 0, 16);
            uint16_t family = 1; /* ARPHRD_ETHER */
            memcpy(request + 16, &family, sizeof(family));
            memcpy(request + 18, mac, 6);
            *result = 0;
            return 1;
        }
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
    if (operation == 15) {
        if (args[1] != 1 || (args[2] != 7 && args[2] != 8)) { *result = -38; return 1; }
        if (!nr_memory(args[4], 4, 1)) { *result = -14; return 1; }
        int capacity; memcpy(&capacity, (const void *)args[4], 4);
        if (capacity < 0) { *result = -22; return 1; }
        int bytes = capacity < 4 ? capacity : 4;
        if (bytes && !nr_memory(args[3], (size_t)bytes, 1)) { *result = -14; return 1; }
        int value, length = sizeof(value);
        int option = args[2] == 7 ? SO_SNDBUF : SO_RCVBUF;
        if (getsockopt(s, SOL_SOCKET, option, (char *)&value, &length)) { *result = socket_error(); return 1; }
        if (bytes) memcpy((void *)args[3], &value, (size_t)bytes);
        memcpy((void *)args[4], &bytes, 4);
        *result = 0; return 1;
    }
    if (operation == 14) {
        if (args[1] == 1 && args[2] == 13) {
            if (args[4] < 8) { *result = -22; return 1; }
            if (!nr_memory(args[3], 8, 0)) { *result = -14; return 1; }
            int32_t input[2]; memcpy(input, (const void *)args[3], sizeof(input));
            if (input[1] < 0 || input[1] > USHRT_MAX) { *result = -38; return 1; }
            /* Linux uses two 32-bit ints; Winsock uses two 16-bit fields. */
            struct linger option = {(u_short)(input[0] != 0), (u_short)input[1]};
            *result = setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&option, sizeof(option)) == 0 ? 0 : socket_error();
            return 1;
        }
        if (args[1] == 1 && (args[2] == 20 || args[2] == 21)) {
            if (args[4] != 2 * sizeof(uintptr_t)) { *result = -22; return 1; }
            if (!nr_memory(args[3], args[4], 0)) { *result = -14; return 1; }
            intptr_t duration[2]; memcpy(duration, (const void *)args[3], sizeof(duration));
            if (duration[0] < 0 || duration[1] < 0 || duration[1] >= 1000000) { *result = -22; return 1; }
            uint64_t ms = (uint64_t)duration[0] * 1000 + ((uint64_t)duration[1] + 999) / 1000;
            uint64_t microseconds = (uint64_t)duration[0] * 1000000 + (uint64_t)duration[1];
            uint64_t *saved = args[2] == 20 ? &run->receive_timeout[fd] : &run->send_timeout[fd];
            *saved = microseconds;
            if (ms > MAXDWORD - 1) {
                *result = 0; return 1;
            }
            DWORD value = (DWORD)ms;
            int option = args[2] == 20 ? SO_RCVTIMEO : SO_SNDTIMEO;
            *result = setsockopt(s, SOL_SOCKET, option, (const char *)&value, sizeof(value)) == 0 ? 0 : socket_error();
            if (*result) *saved = 0;
            return 1;
        }
        if (args[1] == 0 && args[2] == 1 && args[4] == 4) {
            if (!nr_memory(args[3], 4, 0)) { *result = -14; return 1; }
            *result = setsockopt(s, IPPROTO_IP, IP_TOS, (const char *)args[3], 4) == 0 ? 0 : socket_error(); return 1;
        }
        if (args[1] == 6 && args[2] == 1 && args[4] == 4) {
            if (!nr_memory(args[3], 4, 0)) { *result = -14; return 1; }
            *result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)args[3], 4) == 0 ? 0 : socket_error(); return 1;
        }
        if (args[1] != 1 || args[4] != 4 || !nr_memory(args[3], 4, 0)) { *result = -38; return 1; }
        int option;
        switch (args[2]) { case 2: option = SO_REUSEADDR; break; case 6: option = SO_BROADCAST; break;
            case 7: option = SO_SNDBUF; break; case 8: option = SO_RCVBUF; break;
            case 9: option = SO_KEEPALIVE; break; default: *result = -38; return 1; }
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
        if (r == SOCKET_ERROR) { *result = socket_io_error(); return 1; }
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
