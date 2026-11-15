// Instance maintenance for the separately delivered official MySQL tools.
#include <windows.h>
#include <bcrypt.h>
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
#include "../win32_files.h"

static std::wstring wide(const char *text) {
    return std::wstring(text, text + std::strlen(text));
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

static std::wstring prepare(const std::wstring &instance) {
    std::wstring root = instance + L"\\mysql";
    for (const wchar_t *tool : {L"mysqld", L"mysql", L"mysqladmin", L"mysqldump"})
        require(GetFileAttributesW((root + L"\\bin\\" + tool + L".exe").c_str()) != INVALID_FILE_ATTRIBUTES,
                "This instance's official MySQL programs are missing");
    return root;
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
            std::fputs("mysql-control: terminating unfinished owned child after failure\n", stderr);
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
    std::string password = "uu5!^%jg";
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
    if (lost_ack) std::fputs("mysql-control: shutdown client reported ERROR 2013; owned server exited 0 with Shutdown complete.\n", stderr);
    require(DeleteFileW((instance + L"\\bootstrap.private.cnf").c_str()), "Remove temporary bootstrap credential");
    std::puts("Initialized official MySQL 5.7.44; instance credential saved in client.private.cnf; server stopped.");
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
            std::puts("dnf-native MySQL instance controller; instance-local official MySQL 5.7.44");
            return 0;
        }
        if (argc < 3) throw std::runtime_error("Usage: mysql-control.exe initialize INSTANCE [PORT] | backup|restore INSTANCE BACKUP_DIRECTORY | recover INSTANCE | prepare|mysqld|mysql|mysqladmin|mysqldump INSTANCE [official tool arguments]");
        std::wstring tool = argv[1];
        bool maintenance = tool == L"backup" || tool == L"restore" || tool == L"recover";
        if (!maintenance && tool != L"initialize" && tool != L"prepare" && tool != L"mysqld" && tool != L"mysql" && tool != L"mysqladmin" && tool != L"mysqldump")
            throw std::runtime_error("Unknown MySQL operation");
        if (maintenance && argc != (tool == L"recover" ? 3 : 4))
            throw std::runtime_error("Usage: backup|restore INSTANCE BACKUP_DIRECTORY | recover INSTANCE");
        unsigned port = 3306;
        if (tool == L"initialize") {
            if (argc > 4) throw std::runtime_error("Usage: mysql-control.exe initialize INSTANCE [PORT]");
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
            // Recover the cold directory layout even when external tools are
            // unavailable. No official process is needed here.
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
        std::fprintf(stderr, "mysql-control: %s\n", error.what());
        return 1;
    }
}
