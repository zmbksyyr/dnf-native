// Independent bootstrap for the pinned official MySQL tools.
#include <windows.h>
#include <bcrypt.h>
#include <fdi.h>
#include <shlobj.h>
#include <sddl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>
#include "win32_files.h"

struct Entry { const char *name; ULONG bytes; const char *sha256; };
#include "payload_manifest.h"

static std::wstring wide(const char *ascii) {
    std::wstring result(ascii, ascii + std::strlen(ascii));
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
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

class Child {
    HANDLE process;
    HANDLE job = nullptr;
public:
    explicit Child(HANDLE value) : process(value) {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process)) {
            DWORD error = GetLastError();
            TerminateProcess(process, 1);
            CloseHandle(process);
            if (job) CloseHandle(job);
            SetLastError(error);
            require(false, "Own MySQL child job");
        }
    }
    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    Child(Child &&other) noexcept : process(other.process), job(other.job) {
        other.process = nullptr;
        other.job = nullptr;
    }
    bool running() const { return WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
    int wait(DWORD timeout) {
        DWORD result = WaitForSingleObject(process, timeout);
        if (result == WAIT_TIMEOUT) throw std::runtime_error("Official MySQL tool timed out");
        require(result == WAIT_OBJECT_0, "Wait for MySQL tool");
        DWORD code = 1;
        require(GetExitCodeProcess(process, &code), "Read MySQL tool exit status");
        return static_cast<int>(code);
    }
    ~Child() {
        if (!process) return;
        if (running()) {
            std::fputs("mysql-standalone: terminating unfinished owned child after failure\n", stderr);
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
        }
        CloseHandle(process);
        CloseHandle(job);
    }
};

static Child launch(const std::wstring &root, const std::wstring &instance, const std::wstring &tool,
                    const std::vector<std::wstring> &arguments, HANDLE input, HANDLE output, HANDLE error) {
    std::wstring application = root + L"\\bin\\" + tool + L".exe";
    std::wstring command = quote(application);
    for (const std::wstring &argument : arguments) command += L" " + quote(argument);
    if (tool == L"mysqld") command += L" " + quote(L"--basedir=" + root) + L" " + quote(L"--datadir=" + instance + L"\\data");
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = error;
    PROCESS_INFORMATION process = {};
    require(CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                           nullptr, instance.c_str(), &startup, &process), "Start official MySQL tool");
    // Assign the suspended process before it can write data. Closing this
    // wrapper, including an interrupted restore, then also stops its child.
    try {
        Child child(process.hProcess);
        require(ResumeThread(process.hThread) != DWORD(-1), "Resume owned MySQL tool");
        CloseHandle(process.hThread);
        return child;
    } catch (...) { CloseHandle(process.hThread); throw; }
}

static int run(const std::wstring &root, const std::wstring &instance, const std::wstring &tool, int count, wchar_t **arguments) {
    Child child = launch(root, instance, tool, {arguments, arguments + count}, GetStdHandle(STD_INPUT_HANDLE),
                         GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE));
    return child.wait(INFINITE);
}

static std::string random_secret() {
    BYTE bytes[24];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw std::runtime_error("Generate instance secret failed");
    std::string result;
    for (BYTE value : bytes) { result += "0123456789abcdef"[value >> 4]; result += "0123456789abcdef"[value & 15]; }
    return result;
}

static int initialize(const std::wstring &root, const std::wstring &instance, unsigned port) {
    // This operation only accepts a fresh instance; failures retain its data and private diagnostics.
    for (const wchar_t *name : {L"data", L"server.cnf", L"client.private.cnf", L"bootstrap.private.cnf"})
        if (GetFileAttributesW((instance + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("Initialize requires a fresh instance without data or database configuration");
    PSECURITY_DESCRIPTOR descriptor = private_security();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    bool created = CreateDirectoryW((instance + L"\\data").c_str(), &security);
    LocalFree(descriptor);
    require(created, "Create private data directory");
    std::string password = random_secret();
    std::string server_config = "[mysqld]\nbind-address=127.0.0.1\nport=" + std::to_string(port) +
                                "\nlower_case_table_names=1\ncharacter_set_server=latin1\n"
                                "sql_mode=NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION\n";
    write_private(instance + L"\\server.cnf", server_config);
    write_private(instance + L"\\client.private.cnf", "[client]\nprotocol=TCP\nhost=127.0.0.1\nport=" +
                  std::to_string(port) + "\nuser=root\npassword=" + password + "\nssl-mode=REQUIRED\n");
    File input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(SetHandleInformation(input.handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT), "Prepare child input");
    {
        File log = private_file(instance + L"\\mysql-initialize.private.log");
        Child child = launch(root, instance, L"mysqld", {L"--defaults-file=server.cnf", L"--initialize", L"--console"},
                             input.handle, log.handle, log.handle);
        if (child.wait(120000)) throw std::runtime_error("Secure initialization failed; see private initialize log");
    }
    std::string log = read_file(instance + L"\\mysql-initialize.private.log");
    const std::string marker = "A temporary password is generated for root@localhost: ";
    size_t start = log.find(marker);
    if (start == std::string::npos) throw std::runtime_error("Official initialization temporary credential is missing");
    start += marker.size();
    std::string temporary = log.substr(start, log.find_first_of("\r\n", start) - start);
    if (temporary.empty()) throw std::runtime_error("Official initialization temporary credential is empty");
    std::string escaped;
    for (char value : temporary) { if (value == '\\' || value == '"') escaped += '\\'; escaped += value; }
    std::string pipe = "DNFInitialize" + random_secret();
    std::string bootstrap = "[client]\nprotocol=PIPE\nsocket=" + pipe + "\nuser=root\npassword=\"";
    write_private(instance + L"\\bootstrap.private.cnf", bootstrap + escaped + "\"\n");
    File server_log = private_file(instance + L"\\mysql-bootstrap.private.log");
    Child server = launch(root, instance, L"mysqld", {L"--defaults-file=server.cnf", L"--skip-networking",
                          L"--enable-named-pipe", L"--socket=" + wide(pipe.c_str()), L"--console"},
                          input.handle, server_log.handle, server_log.handle);
    auto sql = [&](const std::string &query, HANDLE output = INVALID_HANDLE_VALUE) {
        if (output == INVALID_HANDLE_VALUE) output = server_log.handle;
        SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, TRUE};
        HANDLE reader, writer;
        require(CreatePipe(&reader, &writer, &pipe_security, 0), "Create private SQL input");
        File read_end(reader), write_end(writer);
        require(SetHandleInformation(writer, HANDLE_FLAG_INHERIT, 0), "Protect SQL input writer");
        Child client = launch(root, instance, L"mysql", {L"--defaults-file=bootstrap.private.cnf", L"--connect-expired-password",
                              L"--batch", L"--skip-column-names"}, reader, output, output);
        DWORD bytes = 0;
        require(WriteFile(writer, query.data(), static_cast<DWORD>(query.size()), &bytes, nullptr) && bytes == query.size(),
                "Send private initialization SQL");
        // mysql consumes stdin until EOF. Close the parent writer before waiting.
        CloseHandle(write_end.handle);
        write_end.handle = INVALID_HANDLE_VALUE;
        return client.wait(10000);
    };
    ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        if (!server.running()) throw std::runtime_error("Private bootstrap server exited; see private bootstrap log");
        if (sql("ALTER USER 'root'@'localhost' IDENTIFIED BY '" + password + "';\n") == 0) break;
        if (GetTickCount64() >= deadline) throw std::runtime_error("Temporary credential activation timed out; private files retained");
        Sleep(100);
    }
    write_private(instance + L"\\bootstrap.private.cnf", bootstrap + password + "\"\n", CREATE_ALWAYS);
    if (sql("SELECT VERSION(),@@server_uuid,@@lower_case_table_names;\n"))
        throw std::runtime_error("New credential verification failed; private files retained");
    File shutdown_log = private_file(instance + L"\\mysql-shutdown.private.log");
    int stop_code = sql("SHUTDOWN;\n", shutdown_log.handle);
    int server_code = server.wait(30000);
    bool complete = !server_code && read_file(instance + L"\\mysql-bootstrap.private.log").find("Shutdown complete") != std::string::npos;
    bool lost_ack = stop_code == 1 && read_file(instance + L"\\mysql-shutdown.private.log").find("ERROR 2013 (HY000)") != std::string::npos;
    if (!complete || (stop_code && !lost_ack))
        throw std::runtime_error("Initialized database shutdown failed (client " + std::to_string(stop_code) +
                                 ", server " + std::to_string(server_code) + ")");
    if (lost_ack) std::fputs("mysql-standalone: shutdown client reported ERROR 2013; owned server exited 0 with Shutdown complete.\n", stderr);
    require(DeleteFileW((instance + L"\\bootstrap.private.cnf").c_str()), "Remove temporary bootstrap credential");
    std::puts("Initialized official MySQL 5.7.44; generated credential saved in client.private.cnf; server stopped.");
    return 0;
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

#include "mysql_operations.h"

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--version") {
            std::printf("dnf-native MySQL bootstrap: official MySQL 5.7.44, payload %s\n", payload_id);
            return 0;
        }
        if (argc < 3) throw std::runtime_error("Usage: mysql-standalone.exe initialize INSTANCE [PORT] | backup|restore INSTANCE BACKUP_DIRECTORY | recover INSTANCE | prepare|mysqld|mysql|mysqladmin|mysqldump INSTANCE [official tool arguments]");
        std::wstring tool = argv[1];
        bool maintenance = tool == L"backup" || tool == L"restore" || tool == L"recover";
        if (!maintenance && tool != L"initialize" && tool != L"prepare" && tool != L"mysqld" && tool != L"mysql" && tool != L"mysqladmin" && tool != L"mysqldump")
            throw std::runtime_error("Unknown MySQL operation");
        if (maintenance && argc != (tool == L"recover" ? 3 : 4))
            throw std::runtime_error("Usage: backup|restore INSTANCE BACKUP_DIRECTORY | recover INSTANCE");
        unsigned port = 3306;
        if (tool == L"initialize") {
            if (argc > 4) throw std::runtime_error("Usage: mysql-standalone.exe initialize INSTANCE [PORT]");
            if (argc == 4) {
                size_t used = 0;
                unsigned long value = std::stoul(argv[3], &used);
                if (used != std::wcslen(argv[3]) || value < 1 || value > 65535) throw std::runtime_error("Invalid database port");
                port = static_cast<unsigned>(value);
            }
        }
        std::vector<wchar_t> path(32768);
        DWORD length = GetFullPathNameW(argv[2], static_cast<DWORD>(path.size()), path.data(), nullptr);
        require(length && length < path.size(), "Resolve instance directory");
        std::wstring instance(path.data(), length);
        if (tool == L"prepare") { print_path(prepare(instance)); return 0; }
        if (tool == L"mysqld" || tool == L"initialize" || maintenance) {
            directory(instance + L"\\.mysql");
            File lock(CreateFileW((instance + L"\\.mysql\\operation.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            // Recover the cold directory layout even when payload extraction or
            // verification is unavailable. No official process is needed here.
            if (tool == L"recover") return recover_restore(instance);
            if (exists(instance + L"\\restore.pending"))
                throw std::runtime_error("Interrupted restore requires recover INSTANCE before starting or maintaining this database");
            std::wstring root = prepare(instance);
            if (tool == L"backup") return backup_database(root, instance, absolute_path(argv[3]));
            if (tool == L"restore") return restore_database(root, instance, absolute_path(argv[3]));
            if (tool == L"initialize") return initialize(root, instance, port);
            return run(root, instance, tool, argc - 3, argv + 3);
        }
        return run(prepare(instance), instance, tool, argc - 3, argv + 3);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mysql-standalone: %s\n", error.what());
        return 1;
    }
}
