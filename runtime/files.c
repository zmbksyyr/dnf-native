#include "run.h"
#include <string.h>
#include <stdio.h>

static intptr_t file_error(void) {
    switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return -2;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return -13;
        case ERROR_INVALID_HANDLE: return -9;
        case ERROR_DIRECTORY: return -20;
        case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return -17;
        default: return -5;
    }
}

int nr_path(NrRun *run, const char *guest, wchar_t *path) {
    if (!run->root[0]) return -38;
    char combined[8192];
    if (guest[0] != '/') {
        size_t a = strlen(run->cwd), b = strlen(guest);
        if (a + b + 2 > sizeof(combined)) return -36;
        memcpy(combined, run->cwd, a); combined[a] = '/'; memcpy(combined + a + 1, guest, b + 1);
        guest = combined;
    }
    wchar_t relative[4096];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, guest, -1, relative, 4096)) return -36;
    size_t length = wcslen(run->root);
    wcscpy(path, run->root);
    size_t root_length = length;
    for (wchar_t *p = relative; *p;) {
        while (*p == L'/') ++p;
        wchar_t *part = p;
        while (*p && *p != L'/') {
            if (*p == L'\\' || *p == L':' || *p == L'*' || *p == L'?') return -22;
            ++p;
        }
        size_t n = (size_t)(p - part);
        if (!n || (n == 1 && part[0] == L'.')) continue;
        if (n == 2 && part[0] == L'.' && part[1] == L'.') {
            while (length > root_length && path[length - 1] != L'\\') --length;
            if (length > root_length) --length;
            path[length] = 0;
            continue;
        }
        if (length + n + 2 > 32768) return -36;
        path[length++] = L'\\';
        memcpy(path + length, part, n * sizeof(wchar_t));
        length += n;
        path[length] = 0;
    }
    return 0;
}

static int guest_path(NrRun *run, uintptr_t address, wchar_t *path) {
    char name[4096];
    for (size_t i = 0; i < sizeof(name); ++i) {
        if (!nr_memory(address + i, 1, 0)) return -14;
        name[i] = ((const char *)address)[i];
        if (!name[i]) return i ? nr_path(run, name, path) : -2;
    }
    return -36;
}

static void put32(unsigned char *bytes, size_t offset, uint32_t value) { memcpy(bytes + offset, &value, 4); }
static void put64(unsigned char *bytes, size_t offset, uint64_t value) { memcpy(bytes + offset, &value, 8); }

static uint64_t time_ns(FILETIME time) {
    uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return ticks > 116444736000000000ULL ? (ticks - 116444736000000000ULL) * 100 : 0;
}

static intptr_t stat_file(HANDLE file, uintptr_t destination, int legacy) {
#ifdef _WIN64
    enum {BYTES = 144};
#else
    enum {BYTES = 96};
#endif
    size_t output_size = legacy ? 64 : BYTES;
    if (!nr_memory(destination, output_size, 1)) return -14;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info)) return file_error();
    unsigned char bytes[BYTES] = {0};
    uint64_t inode = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    uint64_t size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    uint32_t mode = info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 0040555 : 0100444;
    FILETIME times[] = {info.ftLastAccessTime, info.ftLastWriteTime, info.ftCreationTime};
#ifdef _WIN64
    put64(bytes, 0, info.dwVolumeSerialNumber); put64(bytes, 8, inode);
    put64(bytes, 16, info.nNumberOfLinks); put32(bytes, 24, mode);
    put32(bytes, 28, 1000); put32(bytes, 32, 1000);
    put64(bytes, 48, size); put64(bytes, 56, 4096); put64(bytes, 64, (size + 511) / 512);
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t ns = time_ns(times[i]);
        put64(bytes, 72 + i * 16, ns / 1000000000); put64(bytes, 80 + i * 16, ns % 1000000000);
    }
#else
    put64(bytes, 0, info.dwVolumeSerialNumber); put32(bytes, 12, (uint32_t)inode);
    put32(bytes, 16, mode); put32(bytes, 20, info.nNumberOfLinks);
    put32(bytes, 24, 1000); put32(bytes, 28, 1000);
    put64(bytes, 44, size); put32(bytes, 52, 4096); put64(bytes, 56, (size + 511) / 512);
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t ns = time_ns(times[i]);
        put32(bytes, 64 + i * 8, (uint32_t)(ns / 1000000000)); put32(bytes, 68 + i * 8, (uint32_t)(ns % 1000000000));
    }
    put64(bytes, 88, inode);
#endif
    if (legacy) {
        if (size > INT32_MAX || inode > UINT32_MAX) return -75;
        memset(bytes, 0, sizeof(bytes));
        put32(bytes, 0, info.dwVolumeSerialNumber); put32(bytes, 4, (uint32_t)inode);
        uint16_t fields[] = {(uint16_t)mode, (uint16_t)info.nNumberOfLinks, 1000, 1000};
        memcpy(bytes + 8, fields, sizeof(fields));
        put32(bytes, 20, (uint32_t)size); put32(bytes, 24, 4096); put32(bytes, 28, (uint32_t)((size + 511) / 512));
        for (unsigned i = 0; i < 3; ++i) {
            uint64_t ns = time_ns(times[i]);
            put32(bytes, 32 + i * 8, (uint32_t)(ns / 1000000000)); put32(bytes, 36 + i * 8, (uint32_t)(ns % 1000000000));
        }
    }
    memcpy((void *)destination, bytes, output_size);
    return 0;
}

int nr_files(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
#ifdef _WIN64
    enum {READ = 0, OPEN = 2, OPENAT = 257, CLOSE = 3, STAT = 4, FSTAT = 5, LSEEK = 8, ACCESS = 21, CHDIR = 80, GETCWD = 79, MKDIR = 83, UNLINK = 87};
#else
    enum {READ = 3, OPEN = 5, OPENAT = 295, CLOSE = 6, STAT = 195, FSTAT = 197, LSEEK = 19, ACCESS = 33, CHDIR = 12, GETCWD = 183, MKDIR = 39, UNLINK = 10};
#endif
    uintptr_t a = args[0], b = args[1], c = args[2];
    int legacy = 0;
#ifndef _WIN64
    if (number == 106) { number = STAT; legacy = 1; }
    if (number == 108) { number = FSTAT; legacy = 1; }
    if (number == 140) {
        if (a >= 256 || !run->files[a]) { *result = -9; return 1; }
        if (!nr_memory(args[3], 8, 1)) { *result = -14; return 1; }
        LARGE_INTEGER offset, position;
        offset.QuadPart = (LONGLONG)(((uint64_t)b << 32) | c);
        if (args[4] > 2) *result = -22;
        else if (!SetFilePointerEx(run->files[a], offset, &position, (DWORD)args[4])) *result = file_error();
        else { memcpy((void *)args[3], &position.QuadPart, 8); *result = 0; }
        return 1;
    }
#endif
    if (number == OPENAT) {
        if ((intptr_t)a != -100) { *result = -38; return 1; }
        number = OPEN; a = b; b = c; c = args[3];
    }
    if (number == GETCWD) {
        size_t bytes = strlen(run->cwd) + 1;
        *result = b < bytes ? -34 : !nr_memory(a, bytes, 1) ? -14 : (intptr_t)bytes;
        if (*result > 0) memcpy((void *)a, run->cwd, bytes);
        return 1;
    }
    if (number == OPEN || number == ACCESS || number == STAT || number == CHDIR || number == MKDIR || number == UNLINK) {
        wchar_t path[32768];
        *result = guest_path(run, a, path);
        if (*result) return 1;
        if (number == UNLINK) {
            *result = DeleteFileW(path) ? 0 : file_error();
            return 1;
        }
        if (number == MKDIR) {
            *result = CreateDirectoryW(path, NULL) ? 0 : file_error();
            return 1;
        }
        if (number == CHDIR) {
            DWORD attributes = GetFileAttributesW(path);
            if (attributes == INVALID_FILE_ATTRIBUTES) { *result = file_error(); return 1; }
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { *result = -20; return 1; }
            wchar_t *relative = path + wcslen(run->root);
            if (!*relative) strcpy(run->cwd, "/");
            else {
                for (wchar_t *p = relative; *p; ++p) if (*p == L'\\') *p = L'/';
                if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative, -1, run->cwd, sizeof(run->cwd), NULL, NULL)) {
                    *result = -36; return 1;
                }
            }
            *result = 0; return 1;
        }
        if (number == STAT) {
            HANDLE file = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (file == INVALID_HANDLE_VALUE) { *result = file_error(); return 1; }
            *result = stat_file(file, b, legacy);
            CloseHandle(file);
            return 1;
        }
        if (number == ACCESS) {
            if (b & ~4u) { *result = -38; return 1; }
            *result = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? file_error() : 0;
            return 1;
        }
        if ((b & 3) == 3) { *result = -22; return 1; }
        if (b & ~(uintptr_t)(3 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x8000 | 0x80000 | 0x10000)) { *result = -38; return 1; }
        if ((b & 0x200) && !(b & 3)) { *result = -38; return 1; }
        unsigned fd;
        for (fd = 0; fd < 256 && run->file_access[fd]; ++fd) {}
        if (fd == 256) { *result = -24; return 1; }
        DWORD desired = (b & 3) != 1 ? GENERIC_READ : 0;
        if (b & 3) desired |= b & 0x400 ? FILE_APPEND_DATA : GENERIC_WRITE;
        DWORD creation = b & 0x40 ? (b & 0x80 ? CREATE_NEW : b & 0x200 ? CREATE_ALWAYS : OPEN_ALWAYS) : b & 0x200 ? TRUNCATE_EXISTING : OPEN_EXISTING;
        HANDLE file = CreateFileW(path, desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, creation, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            *result = file_error();
            fprintf(stderr, "open failed path=%ls flags=0x%llx errno=%lld\n", path, (unsigned long long)b, (long long)*result);
            return 1;
        }
        if (b & 0x10000) {
            BY_HANDLE_FILE_INFORMATION info;
            if (!GetFileInformationByHandle(file, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                CloseHandle(file); *result = -20; return 1;
            }
        }
        run->files[fd] = file;
        run->file_access[fd] = (b & 3) == 0 ? 1 : (b & 3) == 1 ? 2 : 3;
        *result = fd;
        return 1;
    }
    if (number != READ && number != CLOSE && number != FSTAT && number != LSEEK) return 0;
    HANDLE file = a < 256 ? run->files[a] : NULL;
    if (!file) { *result = -9; return 1; }
    if (number == CLOSE) {
        *result = CloseHandle(file) ? 0 : file_error();
        if (!*result) { run->files[a] = NULL; run->file_access[a] = 0; }
    } else if (number == FSTAT) {
        *result = stat_file(file, b, legacy);
    } else if (number == LSEEK) {
        LARGE_INTEGER offset, position;
        offset.QuadPart = (intptr_t)b;
        *result = c > 2 ? -22 : SetFilePointerEx(file, offset, &position, (DWORD)c) ? (intptr_t)position.QuadPart : file_error();
    } else {
        if (!(run->file_access[a] & 1)) { *result = -9; return 1; }
        if (c > 0x7ffff000) c = 0x7ffff000;
        if (!nr_memory(b, c, 1)) { *result = -14; return 1; }
        DWORD count = 0;
        *result = ReadFile(file, (void *)b, (DWORD)c, &count, NULL) ? (intptr_t)count : file_error();
    }
    return 1;
}
