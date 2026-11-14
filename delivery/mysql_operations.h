// Native offline database operations. Included after the single-EXE helpers.
// Backup format 1 restores this instance with unchanged connection configuration.

static bool exists(const std::wstring &path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring absolute_path(const wchar_t *value) {
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetFullPathNameW(value, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    require(length && length < buffer.size(), "Resolve maintenance path");
    std::wstring result(buffer.data(), length);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}

static void private_directory(const std::wstring &path) {
    PSECURITY_DESCRIPTOR descriptor = private_security();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    bool created = CreateDirectoryW(path.c_str(), &security);
    LocalFree(descriptor);
    require(created, "Create private maintenance directory");
}

static File input_file(const std::wstring &path) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    return File(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
}

static std::string file_digest(const std::wstring &path) {
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    Digest digest;
    BYTE buffer[65536];
    for (;;) {
        DWORD bytes = 0;
        require(ReadFile(file.handle, buffer, sizeof(buffer), &bytes, nullptr), "Hash maintenance file");
        if (!bytes) return digest.finish();
        digest.add(buffer, bytes);
    }
}

static void cold_database(const std::wstring &instance) {
    // An unwrapped official server must also release the InnoDB data file.
    File file(CreateFileW((instance + L"\\data\\ibdata1").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

static void copy_tree(const std::wstring &source, const std::wstring &target) {
    require(!(GetFileAttributesW(source.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "Snapshot cannot follow a directory link");
    private_directory(target);
    WIN32_FIND_DATAW item;
    HANDLE search = FindFirstFileW((source + L"\\*").c_str(), &item);
    require(search != INVALID_HANDLE_VALUE, "Enumerate cold data directory");
    try {
        do {
            if (!std::wcscmp(item.cFileName, L".") || !std::wcscmp(item.cFileName, L"..")) continue;
            require(!(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Snapshot cannot follow a file link");
            std::wstring from = source + L"\\" + item.cFileName, to = target + L"\\" + item.cFileName;
            if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(from, to);
            else {
                require(CopyFileW(from.c_str(), to.c_str(), TRUE), "Copy cold database file");
                {
                    File copied(CreateFileW(to.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
                    require(FlushFileBuffers(copied.handle), "Flush cold database copy");
                }
                if (file_digest(from) != file_digest(to)) throw std::runtime_error("Cold snapshot file hash differs");
            }
        } while (FindNextFileW(search, &item));
        require(GetLastError() == ERROR_NO_MORE_FILES, "Complete cold data enumeration");
    } catch (...) { FindClose(search); throw; }
    FindClose(search);
}

class Maintenance {
    std::wstring root, instance, work, endpoint;
    std::wstring last_output;
    unsigned query_count = 0;
    File input, log;
    Child server;
    bool stopped = false;
    std::vector<std::wstring> options() const {
        // Official 5.7 mysql clients cannot open a Chinese absolute defaults
        // path reliably. launch() already sets this instance as their cwd.
        return {L"--defaults-file=client.private.cnf", L"--protocol=MEMORY", L"--host=localhost",
                L"--shared-memory-base-name=" + endpoint,
                L"--ssl-mode=DISABLED", L"--batch", L"--skip-column-names", L"--comments", L"--binary-mode",
                L"--default-character-set=latin1"};
    }
public:
    Maintenance(const std::wstring &payload, const std::wstring &database, const std::wstring &operation)
        : root(payload), instance(database), work(operation), endpoint(L"DNFMaintenance" + wide(random_secret().c_str())),
          input(input_file(L"NUL")), log(private_file(work + L"\\server.private.log")),
          server(launch(root, instance, L"mysqld", {L"--defaults-file=server.cnf", L"--skip-networking",
                        L"--shared-memory", L"--shared-memory-base-name=" + endpoint, L"--console"}, input.handle, log.handle, log.handle)) {
        ULONGLONG deadline = GetTickCount64() + 45000;
        for (;;) {
            if (!server.running()) throw std::runtime_error("Maintenance MySQL exited during startup; private log retained");
            int code = query("SELECT 1;\n");
            if (!code) break;
            if (GetTickCount64() >= deadline) throw std::runtime_error("Maintenance SQL readiness timed out");
            Sleep(100);
        }
    }
    int import_file(const std::wstring &path) {
        File source = input_file(path);
        last_output = work + L"\\client-" + std::to_wstring(++query_count) + L".private.log";
        File output = private_file(last_output);
        Child client = launch(root, instance, L"mysql", options(), source.handle, output.handle, output.handle);
        return client.wait(600000);
    }
    int query(const std::string &text) {
        write_private(work + L"\\query.private.sql", text, CREATE_ALWAYS);
        return import_file(work + L"\\query.private.sql");
    }
    std::string result() const { return read_file(last_output); }
    std::string identity() {
        if (query("SELECT @@server_uuid;\n")) throw std::runtime_error("Read maintenance database identity failed");
        std::string uuid = result();
        uuid.erase(std::remove(uuid.begin(), uuid.end(), '\r'), uuid.end());
        if (uuid.size() != 37 || uuid.back() != '\n') throw std::runtime_error("Unexpected server UUID result");
        return uuid.substr(0, 36);
    }
    void dump(const std::wstring &path) {
        File output = private_file(path);
        File error = private_file(work + L"\\dump.private.log", CREATE_ALWAYS);
        // Preserve official Windows text stdout bytes. --result-file loses CR
        // bytes in stored programs on the next mysql stdin import.
        Child child = launch(root, instance, L"mysqldump", {
            L"--defaults-file=client.private.cnf", L"--protocol=MEMORY", L"--host=localhost", L"--shared-memory-base-name=" + endpoint,
            L"--ssl-mode=DISABLED", L"--default-character-set=latin1", L"--lock-all-tables", L"--routines",
            L"--events", L"--triggers", L"--hex-blob", L"--skip-comments", L"--set-gtid-purged=OFF",
            // Optimizer statistics regenerate when tables are restored. Stored
            // routines/events are exported once, as definitions; dumping their
            // system rows too would also compare recreated internal timestamps.
            L"--ignore-table=mysql.innodb_index_stats", L"--ignore-table=mysql.innodb_table_stats",
            L"--ignore-table=mysql.proc", L"--ignore-table=mysql.event", L"--all-databases"},
            input.handle, output.handle, error.handle);
        if (child.wait(600000)) throw std::runtime_error("Logical backup failed; private log retained");
        require(FlushFileBuffers(output.handle), "Flush logical backup");
    }
    void stop() {
        if (stopped) return;
        int client_code = query("SHUTDOWN;\n");
        int server_code = server.wait(45000);
        bool complete = !server_code && read_file(work + L"\\server.private.log").find("Shutdown complete") != std::string::npos;
        bool lost_ack = client_code == 1 && result().find("ERROR 2013 (HY000)") != std::string::npos;
        if (!complete || (client_code && !lost_ack)) throw std::runtime_error("Maintenance database did not shut down normally");
        stopped = true;
        if (lost_ack) std::fputs("mysql-standalone: maintenance shutdown acknowledgment lost; owned server exited normally.\n", stderr);
    }
    ~Maintenance() {
        if (!stopped && server.running()) {
            try { stop(); }
            catch (const std::exception &error) { std::fprintf(stderr, "mysql-standalone: %s\n", error.what()); }
        }
    }
};

static std::wstring operation_directory(const std::wstring &instance, const wchar_t *kind) {
    std::wstring path = instance + L"\\.mysql\\" + kind + L"-" + wide(random_secret().c_str());
    private_directory(path);
    return path;
}

static std::string backup_manifest(const std::wstring &instance, const std::string &uuid, const std::wstring &sql) {
    return "DNF-NATIVE-MYSQL-BACKUP-1\n" + uuid + "\n" + file_digest(instance + L"\\client.private.cnf") + "\n" +
           file_digest(instance + L"\\server.cnf") + "\n" + file_digest(sql) + "\n";
}

static void backup_location(const std::wstring &instance, const std::wstring &backup) {
    std::wstring prefix = instance + L"\\";
    if (!_wcsicmp(instance.c_str(), backup.c_str()) || !_wcsnicmp(prefix.c_str(), backup.c_str(), prefix.size()))
        throw std::runtime_error("Backup directory must be outside the instance directory");
}

static int backup_database(const std::wstring &root, const std::wstring &instance, const std::wstring &backup) {
    backup_location(instance, backup);
    cold_database(instance);
    private_directory(backup);
    std::wstring work = operation_directory(instance, L"backup");
    Maintenance server(root, instance, work);
    std::string uuid = server.identity();
    server.dump(backup + L"\\logical.sql");
    server.stop();
    write_private(backup + L"\\manifest.txt", backup_manifest(instance, uuid, backup + L"\\logical.sql"));
    std::puts("Logical backup committed; database stopped. Backup includes MySQL accounts and stored objects.");
    return 0;
}

static int recover_restore(const std::wstring &instance) {
    if (!exists(instance + L"\\restore.pending")) {
        std::puts("No interrupted database restore.");
        return 0;
    }
    std::string leaf = read_file(instance + L"\\restore.pending");
    if (leaf.size() != 56 || leaf.substr(0, 8) != "restore-" ||
        leaf.find_first_not_of("0123456789abcdef", 8) != std::string::npos)
        throw std::runtime_error("Restore journal is invalid; preserve it for manual recovery");
    std::wstring work = instance + L"\\.mysql\\" + wide(leaf.c_str());
    if (exists(work + L"\\previous-data")) {
        if (exists(instance + L"\\data")) {
            cold_database(instance);
            require(MoveFileExW((instance + L"\\data").c_str(), (work + L"\\rejected-data").c_str(), MOVEFILE_WRITE_THROUGH),
                    "Retain rejected restore data");
        }
        require(MoveFileExW((work + L"\\previous-data").c_str(), (instance + L"\\data").c_str(), MOVEFILE_WRITE_THROUGH),
                "Roll back to the previous cold database");
    }
    cold_database(instance);
    require(DeleteFileW((instance + L"\\restore.pending").c_str()), "Complete restore recovery");
    std::puts("Previous cold database restored; database stopped. Operation diagnostics retained.");
    return 0;
}

static int restore_database(const std::wstring &root, const std::wstring &instance, const std::wstring &backup) {
    backup_location(instance, backup);
    cold_database(instance);
    // Hold both inputs against writes/deletion for validation and the import.
    File manifest_input = input_file(backup + L"\\manifest.txt");
    File sql_input = input_file(backup + L"\\logical.sql");
    std::string manifest = read_file(backup + L"\\manifest.txt");
    size_t uuid_end = manifest.find('\n', 26);
    if (manifest.substr(0, 26) != "DNF-NATIVE-MYSQL-BACKUP-1\n" || uuid_end != 62)
        throw std::runtime_error("Unrecognized native database backup");
    std::string uuid = manifest.substr(26, 36);
    if (manifest != backup_manifest(instance, uuid, backup + L"\\logical.sql"))
        throw std::runtime_error("Backup hash or instance connection configuration differs");
    std::wstring work = operation_directory(instance, L"restore");
    {
        Maintenance original(root, instance, work);
        if (original.identity() != uuid) throw std::runtime_error("Backup belongs to a different database instance");
        original.dump(work + L"\\before.sql");
        original.stop();
    }
    std::wstring shadow = work + L"\\shadow";
    private_directory(shadow);
    copy_tree(instance + L"\\data", shadow + L"\\data");
    for (const wchar_t *name : {L"server.cnf", L"client.private.cnf"}) {
        require(CopyFileW((instance + L"\\" + name).c_str(), (shadow + L"\\" + name).c_str(), TRUE), "Copy shadow configuration");
    }
    std::wstring import_work = work + L"\\import";
    private_directory(import_work);
    {
        Maintenance candidate(root, shadow, import_work);
        // Remove schemas absent from the chosen backup as well as old tables.
        // Quote each schema identifier, including any embedded backtick.
        if (candidate.query("SELECT CONCAT('DROP DATABASE `',REPLACE(SCHEMA_NAME,'`','``'),'`;') FROM information_schema.SCHEMATA "
                            "WHERE SCHEMA_NAME NOT IN ('mysql','sys','information_schema','performance_schema');\n"))
            throw std::runtime_error("Enumerate old application schemas failed");
        std::string drops = candidate.result();
        if ((!drops.empty() && candidate.query(drops)) || candidate.import_file(backup + L"\\logical.sql"))
            throw std::runtime_error("Shadow logical restore failed; active database retained");
        candidate.stop();
    }
    // Restart to apply restored system accounts before validating the result.
    std::wstring verify_work = work + L"\\verify";
    private_directory(verify_work);
    {
        Maintenance candidate(root, shadow, verify_work);
        candidate.dump(work + L"\\restored.sql");
        candidate.stop();
    }
    if (file_digest(work + L"\\restored.sql") != file_digest(backup + L"\\logical.sql"))
        throw std::runtime_error("Restored database reexport differs; active database retained");
    std::string leaf(work.begin() + work.find_last_of(L'\\') + 1, work.end());
    write_private(instance + L"\\restore.pending", leaf);
    try {
        require(MoveFileExW((instance + L"\\data").c_str(), (work + L"\\previous-data").c_str(), MOVEFILE_WRITE_THROUGH),
                "Retain original cold database snapshot");
        require(MoveFileExW((shadow + L"\\data").c_str(), (instance + L"\\data").c_str(), MOVEFILE_WRITE_THROUGH),
                "Activate verified logical restore");
        std::wstring active_work = work + L"\\active";
        private_directory(active_work);
        Maintenance active(root, instance, active_work);
        if (active.identity() != uuid) throw std::runtime_error("Activated database identity differs");
        active.stop();
        require(DeleteFileW((instance + L"\\restore.pending").c_str()), "Commit verified database restore");
    } catch (...) {
        recover_restore(instance);
        throw;
    }
    std::puts("Logical restore verified and committed; database stopped. Previous cold snapshot and prebackup retained.");
    print_path(work);
    return 0;
}
