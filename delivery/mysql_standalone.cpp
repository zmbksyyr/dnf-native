// Independent bootstrap for the pinned official MySQL tools.
#include <windows.h>
#include <bcrypt.h>
#include <fdi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

struct Entry { const char *name; ULONG bytes; const char *sha256; };
#include "payload_manifest.h"

static void require(bool ok, const char *operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " (Win32 " + std::to_string(GetLastError()) + ")");
}

static std::wstring wide(const char *ascii) {
    std::wstring result(ascii, ascii + std::strlen(ascii));
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}

static void directory(const std::wstring &path) {
    int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        throw std::runtime_error("Create directory failed (Win32 " + std::to_string(result) + ")");
}

class Digest {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<BYTE> object;
public:
    Digest() {
        ULONG bytes = 0, used = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) ||
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<BYTE *>(&bytes), sizeof(bytes), &used, 0))
            throw std::runtime_error("Open SHA-256 failed");
        object.resize(bytes);
        if (BCryptCreateHash(algorithm, &hash, object.data(), bytes, nullptr, 0, 0))
            throw std::runtime_error("Create SHA-256 failed");
    }
    void add(const BYTE *data, ULONG bytes) {
        if (BCryptHashData(hash, const_cast<BYTE *>(data), bytes, 0)) throw std::runtime_error("SHA-256 update failed");
    }
    std::string finish() {
        BYTE result[32];
        if (BCryptFinishHash(hash, result, sizeof(result), 0)) throw std::runtime_error("SHA-256 finish failed");
        std::string text;
        for (BYTE value : result) { text += "0123456789abcdef"[value >> 4]; text += "0123456789abcdef"[value & 15]; }
        return text;
    }
    ~Digest() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};

static void verify(const std::wstring &root) {
    for (const Entry &entry : payload_files) {
        std::wstring path = root + L"\\" + wide(entry.name);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        require(file != INVALID_HANDLE_VALUE, "Open payload file");
        try {
            LARGE_INTEGER size;
            require(GetFileSizeEx(file, &size) && size.QuadPart == entry.bytes, "Payload file size differs");
            Digest digest;
            std::vector<BYTE> buffer(1024 * 1024);
            for (;;) {
                DWORD bytes = 0;
                require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr), "Read payload file");
                if (!bytes) break;
                digest.add(buffer.data(), bytes);
            }
            if (digest.finish() != entry.sha256) throw std::runtime_error(std::string("Payload hash differs: ") + entry.name);
        } catch (...) { CloseHandle(file); throw; }
        CloseHandle(file);
    }
}

static const BYTE *cabinet_data;
static ULONG cabinet_bytes;
struct CabFile { HANDLE disk; LONG position; };
struct Extraction { std::wstring root; std::vector<bool> seen; std::string error; };

static FNALLOC(cab_alloc) { return std::malloc(cb); }
static FNFREE(cab_free) { std::free(pv); }
static FNOPEN(cab_open) {
    (void)oflag; (void)pmode;
    if (std::strcmp(pszFile, "payload.cab")) return -1;
    auto file = static_cast<CabFile *>(std::malloc(sizeof(CabFile)));
    if (!file) return -1;
    *file = {INVALID_HANDLE_VALUE, 0};
    return reinterpret_cast<INT_PTR>(file);
}
static FNREAD(cab_read) {
    auto file = reinterpret_cast<CabFile *>(hf);
    if (file->disk != INVALID_HANDLE_VALUE) return UINT(-1);
    cb = std::min(cb, static_cast<UINT>(cabinet_bytes - static_cast<ULONG>(file->position)));
    std::memcpy(pv, cabinet_data + file->position, cb);
    file->position += static_cast<LONG>(cb);
    return cb;
}
static FNWRITE(cab_write) {
    auto file = reinterpret_cast<CabFile *>(hf);
    DWORD written = 0;
    return WriteFile(file->disk, pv, cb, &written, nullptr) ? written : UINT(-1);
}
static FNCLOSE(cab_close) {
    auto file = reinterpret_cast<CabFile *>(hf);
    bool ok = file->disk == INVALID_HANDLE_VALUE || CloseHandle(file->disk);
    std::free(file);
    return ok ? 0 : -1;
}
static FNSEEK(cab_seek) {
    auto file = reinterpret_cast<CabFile *>(hf);
    if (file->disk != INVALID_HANDLE_VALUE) return -1;
    LONGLONG position = (seektype == SEEK_SET ? 0 : seektype == SEEK_CUR ? file->position : cabinet_bytes);
    position += dist;
    if (position < 0 || position > cabinet_bytes) return -1;
    return file->position = static_cast<LONG>(position);
}
static FNFDINOTIFY(cab_notify) {
    auto &context = *static_cast<Extraction *>(pfdin->pv);
    try {
        if (fdint == fdintCOPY_FILE) {
            std::string name = pfdin->psz1;
            std::replace(name.begin(), name.end(), '\\', '/');
            size_t index = 0;
            while (index < context.seen.size() && name != payload_files[index].name) ++index;
            if (index == context.seen.size() || context.seen[index] || pfdin->cb != static_cast<LONG>(payload_files[index].bytes))
                throw std::runtime_error("Unexpected cabinet file");
            std::wstring path = context.root + L"\\" + wide(name.c_str());
            directory(path.substr(0, path.find_last_of(L"/\\")));
            auto file = static_cast<CabFile *>(std::malloc(sizeof(CabFile)));
            if (!file) throw std::runtime_error("Allocate cabinet handle failed");
            file->disk = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->disk == INVALID_HANDLE_VALUE) { std::free(file); throw std::runtime_error("Create extracted file failed"); }
            file->position = 0;
            context.seen[index] = true;
            return reinterpret_cast<INT_PTR>(file);
        }
        if (fdint == fdintCLOSE_FILE_INFO) return cab_close(pfdin->hf) == 0;
        if (fdint == fdintNEXT_CABINET) throw std::runtime_error("Multiple cabinets are not supported");
        return 0;
    } catch (const std::exception &error) { context.error = error.what(); return -1; }
}

static void extract(const std::wstring &destination) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    require(resource != nullptr, "Find embedded payload");
    cabinet_bytes = SizeofResource(nullptr, resource);
    cabinet_data = static_cast<const BYTE *>(LockResource(LoadResource(nullptr, resource)));
    require(cabinet_data && cabinet_bytes, "Read embedded payload");
    Digest digest;
    digest.add(cabinet_data, cabinet_bytes);
    if (digest.finish() != payload_cab_sha256) throw std::runtime_error("Embedded payload hash differs");
    ERF error = {};
    HFDI fdi = FDICreate(cab_alloc, cab_free, cab_open, cab_read, cab_write, cab_close, cab_seek, cpu80386, &error);
    require(fdi != nullptr, "Create cabinet decoder");
    Extraction context{destination, std::vector<bool>(sizeof(payload_files) / sizeof(payload_files[0])), {}};
    char name[] = "payload.cab", path[] = "";
    bool ok = FDICopy(fdi, name, path, 0, cab_notify, nullptr, &context);
    FDIDestroy(fdi);
    if (!ok || !std::all_of(context.seen.begin(), context.seen.end(), [](bool seen) { return seen; }))
        throw std::runtime_error("Extract payload failed: " + context.error + " (FDI " + std::to_string(error.erfOper) + ")");
    verify(destination);
}

static std::wstring prepare(const std::wstring &instance) {
    std::wstring parent = instance + L"\\.mysql";
    directory(parent);
    std::wstring lock_path = parent + L"\\prepare.lock";
    HANDLE lock = INVALID_HANDLE_VALUE;
    ULONGLONG deadline = GetTickCount64() + 60000;
    do {
        lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
        Sleep(50);
    } while (GetTickCount64() < deadline);
    require(lock != INVALID_HANDLE_VALUE, "Acquire payload preparation lock");
    std::wstring target = parent + L"\\payload-" + wide(payload_id);
    try {
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) verify(target);
        else {
            std::wstring temporary = target + L".partial-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
            require(CreateDirectoryW(temporary.c_str(), nullptr), "Create extraction directory");
            extract(temporary);
            require(MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH), "Commit verified payload");
        }
    } catch (...) { CloseHandle(lock); throw; }
    CloseHandle(lock);
    return target;
}

static std::wstring quote(const std::wstring &argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t value : argument) {
        if (value == L'\\') { ++slashes; continue; }
        result.append(slashes * (value == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (value == L'\"') result += L'\\';
        result += value;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

static int run(const std::wstring &root, const std::wstring &instance, const std::wstring &tool, int count, wchar_t **arguments) {
    std::wstring application = root + L"\\bin\\" + tool + L".exe";
    std::wstring command = quote(application);
    for (int index = 0; index < count; ++index) command += L" " + quote(arguments[index]);
    if (tool == L"mysqld") command += L" " + quote(L"--basedir=" + root) + L" " + quote(L"--datadir=" + instance + L"\\data");
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    require(CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, instance.c_str(), &startup, &process), "Start official MySQL tool");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    bool ok = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    require(ok, "Read MySQL tool exit status");
    return static_cast<int>(code);
}

static void print_path(const std::wstring &path) {
    std::wstring text = path + L"\n";
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(output, &mode)) { WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr); return; }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::vector<char> encoded(bytes);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), encoded.data(), bytes, nullptr, nullptr);
    WriteFile(output, encoded.data(), bytes, &written, nullptr);
}

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--version") {
            std::printf("dnf-native MySQL bootstrap: official MySQL 5.7.44, payload %s\n", payload_id);
            return 0;
        }
        if (argc < 3) throw std::runtime_error("Usage: mysql-standalone.exe prepare|mysqld|mysql|mysqladmin|mysqldump INSTANCE [official tool arguments]");
        std::wstring tool = argv[1];
        if (tool != L"prepare" && tool != L"mysqld" && tool != L"mysql" && tool != L"mysqladmin" && tool != L"mysqldump")
            throw std::runtime_error("Unknown MySQL operation");
        std::vector<wchar_t> path(32768);
        DWORD length = GetFullPathNameW(argv[2], static_cast<DWORD>(path.size()), path.data(), nullptr);
        require(length && length < path.size(), "Resolve instance directory");
        std::wstring instance(path.data(), length);
        std::wstring root = prepare(instance);
        if (tool == L"prepare") { print_path(root); return 0; }
        return run(root, instance, tool, argc - 3, argv + 3);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mysql-standalone: %s\n", error.what());
        return 1;
    }
}
