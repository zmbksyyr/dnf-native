#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "run.h"
#include <winternl.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

_Static_assert(FD_SETSIZE == NR_FD_SLOTS, "Winsock sets must cover all guest descriptors");

static intptr_t socket_errno(int error) {
    switch (error) {
        case 0: return 0;
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
        case WSAECONNABORTED: return -103;
        case WSAECONNRESET: return -104;
        case WSAECONNREFUSED: return -111;
        case WSAETIMEDOUT: return -110;
        case WSAENETUNREACH: return -101;
        case WSAENETRESET: return -102;
        case WSAEHOSTUNREACH: return -113;
        case WSAEMSGSIZE: return -90;
        case WSAEDESTADDRREQ: return -89;
        case WSAENOPROTOOPT: return -92;
        case WSAENOTCONN: return -107;
        default: return -5;
    }
}

static intptr_t socket_error(void) { return socket_errno(WSAGetLastError()); }

enum { STREAM_ERROR_CONSUMED = 1, STREAM_WRITE_SHUT = 2 };

static intptr_t broken_pipe(int flags) {
    if (!(flags & 0x4000)) { /* Linux MSG_NOSIGNAL. SIGPIPE belongs to this writer. */
        NrThread *thread = nr_current();
        thread->signal_origin[13] = 0;
        thread->signal_pending |= 1ULL << 12;
    }
    return -32;
}

/* Keep Winsock nonblocking. Guest O_NONBLOCK and MSG_DONTWAIT decide whether
   this caller waits; they must never change another thread's socket mode.
   Zero timeout means unlimited; a finite timeout keeps one deadline on retry. */
static intptr_t wait_socket(SOCKET socket, int writing, uint64_t timeout, ULONGLONG started) {
    struct timeval duration, *limit = NULL;
    if (timeout) {
        uint64_t elapsed = (GetTickCount64() - started) * 1000;
        if (elapsed >= timeout) return -11;
        uint64_t remaining = timeout - elapsed;
        duration.tv_sec = remaining / 1000000 > LONG_MAX ? LONG_MAX : (long)(remaining / 1000000);
        duration.tv_usec = (long)(remaining % 1000000);
        limit = &duration;
    }
    fd_set set, errors;
    FD_ZERO(&set); FD_SET(socket, &set);
    FD_ZERO(&errors); FD_SET(socket, &errors);
    int ready = select(0, writing ? NULL : &set, writing ? &set : NULL, &errors, limit);
    if (ready == SOCKET_ERROR) return socket_error();
    return ready ? 0 : -11;
}

static intptr_t socket_transfer(NrRun *run, unsigned fd, void *buffer, int bytes, int receiving,
                                int flags, struct sockaddr *address, int *length) {
    SOCKET s = (SOCKET)run->sockets[fd];
    int stream = run->socket_type[fd] == SOCK_STREAM;
    if (stream && !receiving && (run->stream_state[fd] & (STREAM_ERROR_CONSUMED | STREAM_WRITE_SHUT)))
        return broken_pipe(flags);
    ULONGLONG started = GetTickCount64();
    int r;
    for (;;) {
        r = receiving ? recvfrom(s, buffer, bytes, flags & ~(0x4000 | 0x40), address, length) :
                        sendto(s, buffer, bytes, flags & ~(0x4000 | 0x40), address, address ? 16 : 0);
        if (r != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK || run->nonblock[fd] || (flags & 0x40)) break;
        intptr_t waited = wait_socket(s, !receiving, receiving ? run->receive_timeout[fd] : run->send_timeout[fd], started);
        if (waited) return waited;
    }
    if (r != SOCKET_ERROR) return r;
    int error = WSAGetLastError();
    if (!stream && !receiving && !address && error == WSAENOTCONN) return -89;
    if (stream) {
        /* Winsock retains a terminal connection error on every I/O. Linux
           consumes it once, then reads EOF and writes EPIPE. Keep that state
           per descriptor; UDP errors and asynchronous connect stay separate. */
        if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENETRESET) {
            LONG previous = InterlockedOr(&run->stream_state[fd], STREAM_ERROR_CONSUMED);
            if (previous & STREAM_ERROR_CONSUMED) return receiving ? 0 : broken_pipe(flags);
        }
        if (!receiving && (error == WSAESHUTDOWN || error == WSAENOTCONN)) return broken_pipe(flags);
        if (receiving && error == WSAESHUTDOWN) return 0;
    }
    return error == WSAETIMEDOUT ? -11 : socket_errno(error);
}

static int epoll_trace_enabled(const NrRun *run) { return run->epoll_trace_enabled != 0; }

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

static intptr_t add_socket(NrRun *run, SOCKET s, int nonblock, int type) {
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode)) { intptr_t error = socket_error(); closesocket(s); return error; }
    unsigned fd = nr_fd_reserve(run);
    if (fd == NR_FD_SLOTS) { closesocket(s); return -24; }
    run->sockets[fd] = (uintptr_t)s;
    run->nonblock[fd] = nonblock != 0;
    run->connect_state[fd] = 0;
    run->socket_type[fd] = (unsigned char)type;
    run->stream_state[fd] = 0;
    run->file_access[fd] = 4;
    return fd;
}

static intptr_t select_mixed(NrRun *run, const uintptr_t args[6]);

static intptr_t select_sockets(NrRun *run, const uintptr_t args[6]) {
    if (args[0] > NR_FD_SLOTS) return -22;
    size_t bytes = ((args[0] + sizeof(uintptr_t) * 8 - 1) / (sizeof(uintptr_t) * 8)) * sizeof(uintptr_t);
    unsigned char bits[3][NR_FD_SLOTS / 8] = {{0}};
    SOCKET sockets[NR_FD_SLOTS] = {0};
    fd_set sets[3];
    for (unsigned i = 0; i < 3; ++i) {
        FD_ZERO(&sets[i]);
        if (!args[i + 1]) continue;
        if (!nr_memory(args[i + 1], bytes, 1)) return -14;
        memcpy(bits[i], (const void *)args[i + 1], bytes);
        for (unsigned fd = 0; fd < args[0]; ++fd) {
            if (!(bits[i][fd / 8] & (1u << (fd % 8)))) continue;
            if (run->file_access[fd] == NR_EVENTFD) return select_mixed(run, args);
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
        unsigned char output[NR_FD_SLOTS / 8] = {0};
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
        WSAPOLLFD polls[NR_FD_SLOTS]; unsigned indices[NR_FD_SLOTS]; ULONG count = 0;
        int has_pipes = 0;
        ready = 0;
        for (unsigned i = 0; i < length; ++i) {
            entries[i].revents = 0;
            int fd = entries[i].fd;
            if (fd < 0) continue;
            if (fd >= NR_FD_SLOTS || !run->file_access[fd]) { entries[i].revents = 32; ++ready; continue; }
            if (run->file_access[fd] == NR_EVENTFD) {
                entries[i].revents = (short)nr_eventfd_events(run, (unsigned)fd, entries[i].events);
                if (entries[i].revents) ++ready;
                has_pipes = 1;
                continue;
            }
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
            /* Linux TCP EOF (and reset with HUP) is readable: the next read
               returns zero or the pending error. Winsock reports only HUP,
               so callers that consume disconnects through EPOLLIN would
               otherwise retain dead peers indefinitely. */
            if (polls[i].revents & POLLHUP) events |= 16 | (entries[at].events & (1 | 64));
            if (polls[i].revents & POLLNVAL) events |= 32;
            entries[at].revents = events;
            if (events) ++ready;
        }
        if (ready || expired) break;
    }
    return ready;
}

static intptr_t poll_sockets(NrRun *run, const uintptr_t args[6]) {
    NrPoll entries[NR_FD_SLOTS];
    if (args[1] > NR_FD_SLOTS) return -22;
    size_t bytes = (size_t)args[1] * sizeof(entries[0]);
    if (!nr_memory(args[0], bytes, 1)) return -14;
    memcpy(entries, (const void *)args[0], bytes);
    intptr_t ready = poll_descriptors(run, entries, (unsigned)args[1], (int)args[2]);
    if (ready >= 0) memcpy((void *)args[0], entries, bytes);
    return ready;
}

static intptr_t select_mixed(NrRun *run, const uintptr_t args[6]) {
    size_t bytes = ((args[0] + sizeof(uintptr_t) * 8 - 1) / (sizeof(uintptr_t) * 8)) * sizeof(uintptr_t);
    unsigned char bits[3][NR_FD_SLOTS / 8] = {{0}}, output[3][NR_FD_SLOTS / 8] = {{0}};
    for (unsigned i = 0; i < 3; ++i) if (args[i + 1]) {
        if (!nr_memory(args[i + 1], bytes, 1)) return -14;
        memcpy(bits[i], (const void *)args[i + 1], bytes);
    }
    int timeout = -1;
    uint64_t microseconds = 0;
    if (args[4]) {
        if (!nr_memory(args[4], 2 * sizeof(uintptr_t), 1)) return -14;
        intptr_t input[2]; memcpy(input, (const void *)args[4], sizeof(input));
        if (input[0] < 0 || input[1] < 0 || (uint64_t)input[0] > (UINT64_MAX - (uint64_t)input[1]) / 1000000) return -22;
        microseconds = (uint64_t)input[0] * 1000000 + (uint64_t)input[1];
        if (microseconds / 1000 > INT_MAX - 1) return -38;
        timeout = (int)((microseconds + 999) / 1000);
    }
    NrPoll entries[NR_FD_SLOTS]; unsigned count = 0;
    for (unsigned fd = 0; fd < args[0]; ++fd) {
        unsigned bit = 1u << (fd % 8);
        short events = (bits[0][fd / 8] & bit ? 1 : 0) | (bits[1][fd / 8] & bit ? 4 : 0) | (bits[2][fd / 8] & bit ? 2 : 0);
        if (!events) continue;
        if (run->file_access[fd] != 4 && run->file_access[fd] != NR_EVENTFD) return -9;
        entries[count++] = (NrPoll){(int32_t)fd, events, 0};
    }
    ULONGLONG start = GetTickCount64();
    intptr_t result = poll_descriptors(run, entries, count, timeout);
    if (result < 0) return result;
    result = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned fd = (unsigned)entries[i].fd, bit = 1u << (fd % 8);
        if (entries[i].revents & 32) return -9;
        short ready[] = {(short)(entries[i].revents & (1 | 16 | 8)),
                         (short)(entries[i].revents & (4 | 8)), (short)(entries[i].revents & 2)};
        for (unsigned set = 0; set < 3; ++set) if ((bits[set][fd / 8] & bit) && ready[set]) {
            output[set][fd / 8] |= (unsigned char)bit; ++result;
        }
    }
    for (unsigned i = 0; i < 3; ++i) if (args[i + 1]) memcpy((void *)args[i + 1], output[i], bytes);
    if (args[4]) {
        uint64_t used = (GetTickCount64() - start) * 1000;
        microseconds = used >= microseconds ? 0 : microseconds - used;
        uintptr_t remaining[] = {(uintptr_t)(microseconds / 1000000), (uintptr_t)(microseconds % 1000000)};
        memcpy((void *)args[4], remaining, sizeof(remaining));
    }
    return result;
}

static intptr_t epoll_wait(NrRun *run, unsigned epoll_fd, NrWatch *watches, uintptr_t address,
                           uintptr_t maximum, intptr_t milliseconds) {
    if (!maximum || maximum > INT_MAX || milliseconds < -1) return -22;
    size_t capacity = maximum > NR_FD_SLOTS ? NR_FD_SLOTS : maximum;
    if (!nr_memory(address, capacity * 12, 1)) return -14;
    NrPoll polls[NR_FD_SLOTS]; unsigned count = 0;
    unsigned cursor = run->epoll_cursor[epoll_fd];
    for (unsigned step = 0; step < NR_FD_SLOTS; ++step) {
        unsigned fd = (cursor + step) % NR_FD_SLOTS;
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
        /* Linux rotates the ready list when maxevents truncates a batch.
           Always starting at fd zero starves later sockets under load. */
        run->epoll_cursor[epoll_fd] = ((unsigned)polls[i].fd + 1) % NR_FD_SLOTS;
    }
    if (epoll_trace_enabled(run)) {
        fprintf(stderr, "epoll wait epfd=%u max=%llu timeout=%lld watches=%u ready=%lld returned=%lld\n",
                epoll_fd, (unsigned long long)maximum, (long long)milliseconds, count,
                (long long)ready, (long long)written);
        intptr_t traced = 0;
        for (unsigned i = 0; i < count && traced < written; ++i) {
            if (!(polls[i].revents & 61)) continue;
            uint64_t data;
            memcpy(&data, (const unsigned char *)address + traced * 12 + 4, 8);
            ++traced;
            fprintf(stderr, "epoll event epfd=%u fd=%d revents=0x%x data=0x%llx\n",
                    epoll_fd, polls[i].fd, (unsigned)(unsigned short)polls[i].revents,
                    (unsigned long long)data);
        }
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
        unsigned fd = nr_fd_reserve(run);
        if (fd == NR_FD_SLOTS) { *result = -24; return 1; }
        run->epolls[fd] = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NR_FD_SLOTS * sizeof(NrWatch));
        if (!run->epolls[fd]) { run->file_access[fd] = 0; *result = -12; return 1; }
        run->file_access[fd] = 8;
        run->epoll_cursor[fd] = 0;
        if (epoll_trace_enabled(run)) fprintf(stderr, "epoll create epfd=%u\n", fd);
        *result = fd; return 1;
    }
    if (number == CLOSE && input[0] < NR_FD_SLOTS) {
        unsigned fd = (unsigned)input[0];
        /* Another close walks these sets to remove its descriptor. Keep that
           walk and epoll destruction together so it cannot touch a freed set. */
        EnterCriticalSection(&run->memory_lock);
        if (run->file_access[fd] == 8) {
            HeapFree(GetProcessHeap(), 0, run->epolls[fd]); run->epolls[fd] = NULL; run->file_access[fd] = 0;
            LeaveCriticalSection(&run->memory_lock);
            *result = 0; return 1;
        }
        for (unsigned i = 0; i < NR_FD_SLOTS; ++i) if (run->epolls[i]) run->epolls[i][fd].used = 0;
        LeaveCriticalSection(&run->memory_lock);
    }
    if (number == EP_CTL || number == EP_WAIT) {
        if (input[0] >= NR_FD_SLOTS || run->file_access[input[0]] != 8) { *result = -9; return 1; }
        NrWatch *watches = run->epolls[input[0]];
        if (number == EP_WAIT) {
            *result = epoll_wait(run, (unsigned)input[0], watches, input[1], input[2], (intptr_t)input[3]);
            return 1;
        }
            if (input[2] >= NR_FD_SLOTS || !run->file_access[input[2]]) { *result = -9; return 1; }
        if (run->file_access[input[2]] != 4 && run->file_access[input[2]] != NR_EVENTFD &&
            (!run->files[input[2]] || GetFileType(run->files[input[2]]) != FILE_TYPE_PIPE)) {
            *result = -1; return 1;
        }
        NrWatch *watch = &watches[input[2]];
        if (input[1] == 2) {
            if (epoll_trace_enabled(run)) fprintf(stderr,
                "epoll ctl epfd=%u op=%llu fd=%u used=%u data=0x%llx\n",
                (unsigned)input[0], (unsigned long long)input[1], (unsigned)input[2],
                watch->used, (unsigned long long)watch->data);
            *result = watch->used ? 0 : -2; watch->used = 0; return 1;
        }
        if (input[1] != 1 && input[1] != 3) { *result = -22; return 1; }
        if (!nr_memory(input[3], 12, 0)) { *result = -14; return 1; }
        uint32_t events; memcpy(&events, (const void *)input[3], 4);
        if (events & ~29u) {
            fprintf(stderr, "epoll unsupported events=0x%x\n", events); *result = -38; return 1;
        }
        if ((input[1] == 1 && watch->used) || (input[1] == 3 && !watch->used)) { *result = watch->used ? -17 : -2; return 1; }
        watch->used = 1; watch->events = events; memcpy(&watch->data, (const void *)(input[3] + 4), 8);
        if (epoll_trace_enabled(run)) fprintf(stderr,
            "epoll ctl epfd=%u op=%llu fd=%u events=0x%x data=0x%llx\n",
            (unsigned)input[0], (unsigned long long)input[1], (unsigned)input[2], events,
            (unsigned long long)watch->data);
        *result = 0; return 1;
    }
    uintptr_t args[6]; memcpy(args, input, sizeof(args));
    int operation = 0;
#ifdef _WIN64
    switch (number) {
        case 41: operation = 1; break; case 49: operation = 2; break; case 42: operation = 3; break;
        case 50: operation = 4; break; case 43: operation = 5; break; case 51: operation = 6; break; case 52: operation = 7; break;
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
    if (operation && run->trace_enabled)
        fprintf(stderr, "socket op=%d args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx\n", operation,
                (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2],
                (unsigned long long)args[3], (unsigned long long)args[4], (unsigned long long)args[5]);
    if (!operation && ((number != CLOSE && number != IOCTL && number != FCNTL && number != READ && number != WRITE) || fd >= NR_FD_SLOTS || run->file_access[fd] != 4)) return 0;
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
        *result = s == INVALID_SOCKET ? socket_error() : add_socket(run, s, (args[1] & 0x800) != 0, (int)(args[1] & 15));
        return 1;
    }
    if (fd >= NR_FD_SLOTS || run->file_access[fd] != 4) { *result = -9; return 1; }
    SOCKET s = (SOCKET)run->sockets[fd];
    if (!operation && (number == READ || number == WRITE)) {
        int receiving = number == READ;
        size_t bytes = args[2] > 0x7ffff000 ? 0x7ffff000 : args[2];
        if (!nr_memory(args[1], bytes, receiving)) { *result = -14; return 1; }
        if (!bytes && receiving) { *result = 0; return 1; }
        *result = socket_transfer(run, (unsigned)fd, (void *)args[1], (int)bytes, receiving, 0, NULL, NULL);
        return 1;
    }
    if (number == CLOSE && !operation) {
        /* Publish the closing state before closesocket wakes a blocked select.
           Keep the slot occupied until its remaining state has been cleared. */
        InterlockedExchangePointer((PVOID volatile *)&run->sockets[fd], (PVOID)INVALID_SOCKET);
        *result = closesocket(s) == 0 ? 0 : socket_error();
        if (!*result) {
            run->nonblock[fd] = 0;
            run->connect_state[fd] = 0;
            run->socket_type[fd] = 0;
            run->stream_state[fd] = 0;
            run->receive_timeout[fd] = run->send_timeout[fd] = 0;
            run->file_access[fd] = 0;
        } else run->sockets[fd] = (uintptr_t)s;
        return 1;
    }
    if (number == FCNTL && !operation) {
        if (args[1] == 3) *result = 2 | (run->nonblock[fd] ? 0x800 : 0);
        else if (args[1] == 4 && !(args[2] & ~(uintptr_t)(2 | 0x800))) {
            run->nonblock[fd] = (args[2] & 0x800) != 0;
            *result = 0;
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
        run->nonblock[fd] = mode != 0;
        *result = 0;
        return 1;
    }
    if (operation == 2 || operation == 3) {
        if (args[2] != 16 || !nr_memory(args[1], 16, 0)) { *result = args[2] != 16 ? -22 : -14; return 1; }
        struct sockaddr_in address; memcpy(&address, (const void *)args[1], 16);
        if (address.sin_family != AF_INET) { *result = -97; return 1; }
        if (operation == 3) run->connect_state[fd] = 0;
        int r = operation == 2 ? bind(s, (const struct sockaddr *)&address, 16) : connect(s, (const struct sockaddr *)&address, 16);
        if (r == SOCKET_ERROR && operation == 3 && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (run->nonblock[fd]) { run->connect_state[fd] = 1; *result = -115; }
            else {
                *result = wait_socket(s, 1, run->send_timeout[fd], GetTickCount64());
                if (*result == -11) { run->connect_state[fd] = 1; *result = -115; }
                else if (!*result) {
                    int error = 0, length = sizeof(error);
                    *result = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &length) ? socket_error() : socket_errno(error);
                }
            }
        } else *result = r == 0 ? 0 : socket_error();
        return 1;
    }
    if (operation == 4) { *result = listen(s, (int)args[1]) == 0 ? 0 : socket_error(); return 1; }
    if (operation == 13) {
        *result = shutdown(s, (int)args[1]) == 0 ? 0 : socket_error();
        if (!*result && run->socket_type[fd] == SOCK_STREAM && (args[1] == 1 || args[1] == 2))
            InterlockedOr(&run->stream_state[fd], STREAM_WRITE_SHUT);
        return 1;
    }
    if (operation == 15) {
        if (args[1] != 1 || (args[2] != 4 && args[2] != 7 && args[2] != 8)) {
            fprintf(stderr, "getsockopt unsupported level=%llu option=%llu\n",
                    (unsigned long long)args[1], (unsigned long long)args[2]);
            *result = -38; return 1;
        }
        if (!nr_memory(args[4], 4, 1)) { *result = -14; return 1; }
        int capacity; memcpy(&capacity, (const void *)args[4], 4);
        if (capacity < 0) { *result = -22; return 1; }
        int bytes = capacity < 4 ? capacity : 4;
        if (bytes && !nr_memory(args[3], (size_t)bytes, 1)) { *result = -14; return 1; }
        int value, length = sizeof(value);
        int option = args[2] == 4 ? SO_ERROR : args[2] == 7 ? SO_SNDBUF : SO_RCVBUF;
        if (args[2] == 4 && (run->connect_state[fd] == 2 ||
                            (run->stream_state[fd] & STREAM_ERROR_CONSUMED))) value = 0;
        else {
            if (getsockopt(s, SOL_SOCKET, option, (char *)&value, &length)) { *result = socket_error(); return 1; }
            if (args[2] == 4) {
                /* Winsock can report zero SO_ERROR after a TCP reset while
                   recv still exposes the terminal error. Peek only a socket
                   with terminal readiness, so a healthy blocking socket is
                   never read here and queued data remains untouched. */
                if (!value && run->socket_type[fd] == SOCK_STREAM && !run->connect_state[fd]) {
                    WSAPOLLFD poll = {s, POLLRDNORM, 0};
                    if (WSAPoll(&poll, 1, 0) > 0 && (poll.revents & (POLLERR | POLLHUP))) {
                        char byte;
                        if (recv(s, &byte, 1, MSG_PEEK) == SOCKET_ERROR) {
                            int error = WSAGetLastError();
                            if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENETRESET)
                                value = error;
                        }
                    }
                }
                if (run->socket_type[fd] == SOCK_STREAM && run->connect_state[fd] != 1 &&
                    (value == WSAECONNRESET || value == WSAECONNABORTED || value == WSAENETRESET))
                    InterlockedOr(&run->stream_state[fd], STREAM_ERROR_CONSUMED);
                value = (int)-socket_errno(value);
                /* Winsock retains a failed asynchronous connect's SO_ERROR.
                   Linux consumes it on the first query. A new connect resets it. */
                if (value && run->connect_state[fd] == 1) run->connect_state[fd] = 2;
                else if (!value && run->connect_state[fd] == 1) {
                    struct sockaddr_in peer; int peer_bytes = sizeof(peer);
                    if (!getpeername(s, (struct sockaddr *)&peer, &peer_bytes)) run->connect_state[fd] = 0;
                }
            }
        }
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
    if (operation == 5 || operation == 6 || operation == 7) {
        struct sockaddr_in address; int bytes = sizeof(address);
        int capacity = 0, output = operation != 5 || args[1];
        if (output) {
            if (!nr_memory(args[2], 4, 1)) { *result = -14; return 1; }
            memcpy(&capacity, (const void *)args[2], 4);
            if (capacity < 0) { *result = -22; return 1; }
            if (capacity > bytes) capacity = bytes;
            if (capacity && !nr_memory(args[1], (size_t)capacity, 1)) { *result = -14; return 1; }
        }
        SOCKET accepted = INVALID_SOCKET;
        int r;
        if (operation == 5) {
            ULONGLONG started = GetTickCount64();
            for (;;) {
                accepted = accept(s, (struct sockaddr *)&address, &bytes);
                r = accepted == INVALID_SOCKET ? SOCKET_ERROR : 0;
                if (!r || WSAGetLastError() != WSAEWOULDBLOCK || run->nonblock[fd]) break;
                intptr_t waited = wait_socket(s, 0, run->receive_timeout[fd], started);
                if (waited) { *result = waited; return 1; }
            }
        }
        else r = operation == 6 ? getsockname(s, (struct sockaddr *)&address, &bytes) : getpeername(s, (struct sockaddr *)&address, &bytes);
        if (r == SOCKET_ERROR) { *result = socket_error(); return 1; }
        if (output) {
            if (capacity > 0) memcpy((void *)args[1], &address, (size_t)capacity);
            memcpy((void *)args[2], &bytes, 4);
        }
        *result = operation == 5 ? add_socket(run, accepted, 0, SOCK_STREAM) : 0; return 1;
    }
    if (operation == 9 || operation == 10 || operation == 11 || operation == 12) {
        int receiving = operation == 10 || operation == 12;
        uintptr_t unsupported_flags = args[3] & ~(uintptr_t)(receiving ? 0x40 : 0x4040);
        if (args[2] > INT_MAX || unsupported_flags || !nr_memory(args[1], args[2], receiving)) {
            if (unsupported_flags) fprintf(stderr, "socket unsupported transfer op=%d flags=0x%llx bytes=%llu\n",
                                           operation, (unsigned long long)args[3], (unsigned long long)args[2]);
            *result = unsupported_flags || args[2] > INT_MAX ? -38 : -14; return 1;
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
        intptr_t r = socket_transfer(run, (unsigned)fd, (void *)args[1], (int)args[2], receiving, (int)args[3],
                                     args[4] ? (struct sockaddr *)&address : NULL, args[4] ? &bytes : NULL);
        if (r < 0) { *result = r; return 1; }
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
