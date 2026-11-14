// Native local-resource preparation. Included after database process helpers.
#pragma once
#include <bcrypt.h>
#include <cstdint>
#include "resource_cabinet.h"

static std::string file_sha256(const std::wstring &path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        throw std::runtime_error("Open resource SHA256 provider failed");
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) {
        BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("Create resource SHA256 failed");
    }
    try {
        File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        BYTE buffer[65536], digest[32]; DWORD count;
        for (;;) {
            require(ReadFile(file.handle, buffer, sizeof(buffer), &count, nullptr), "Read resource for SHA256");
            if (!count) break;
            if (BCryptHashData(hash, buffer, count, 0)) throw std::runtime_error("Hash resource failed");
        }
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0)) throw std::runtime_error("Finish resource SHA256 failed");
        std::string result;
        for (BYTE value : digest) { result += "0123456789abcdef"[value >> 4]; result += "0123456789abcdef"[value & 15]; }
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); return result;
    } catch (...) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
}

static std::string ascii(const std::wstring &text) {
    if (std::any_of(text.begin(), text.end(), [](wchar_t c) { return c > 127; })) throw std::runtime_error("Expected ASCII resource metadata");
    return {text.begin(), text.end()};
}

static std::wstring resource_path(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.empty() || path.front() == L'\\' || path.find(L':') != std::wstring::npos ||
        (L"\\" + path + L"\\").find(L"\\..\\") != std::wstring::npos)
        throw std::runtime_error("Resource path must stay inside its directory");
    return path;
}

static void replace_all(std::string &text, const std::string &before, const std::string &after) {
    size_t offset = 0; bool found = false;
    while ((offset = text.find(before, offset)) != std::string::npos) {
        text.replace(offset, before.size(), after); offset += after.size(); found = true;
    }
    if (!found) throw std::runtime_error("Instance configuration template field is missing");
}

static std::string instance_secret(unsigned bytes) {
    std::vector<BYTE> random(bytes);
    if (BCryptGenRandom(nullptr, random.data(), bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw std::runtime_error("Generate application secret failed");
    std::string result;
    for (BYTE value : random) { result += "0123456789abcdef"[value >> 4]; result += "0123456789abcdef"[value & 15]; }
    return result;
}

static std::string legacy_password(const std::wstring &elf, const std::string &password) {
    if (password.empty() || password.size() >= 20) throw std::runtime_error("Original application requires a 1-19 byte password");
    if (file_sha256(elf) != "0979ef164615e5e3eae006fde74ca2a39a920367dae6a143847dbc0a8d49a967")
        throw std::runtime_error("Application credential format requires the recorded original DBMW ELF");
    std::string data = read_file(elf);
    auto le32 = [&](size_t at) -> uint32_t {
        if (at + 4 > data.size()) throw std::runtime_error("ELF credential data outside file");
        const auto *p = reinterpret_cast<const unsigned char *>(data.data() + at);
        return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    auto be32 = [](const unsigned char *p) -> uint32_t {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };
    uint32_t phoff = le32(28), stride = le32(42) & 0xffff, count = le32(42) >> 16;
    uint32_t key[4] = {}; bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = phoff + size_t(stride) * i;
        uint32_t address = le32(at + 8), size = le32(at + 16);
        if (le32(at) != 1 || address > 0x081ab9c0 || uint64_t(address) + size < 0x081ab9d0) continue;
        size_t offset = size_t(le32(at + 4)) + 0x081ab9c0 - address;
        if (offset + 16 > data.size()) throw std::runtime_error("Original credential key is not file backed");
        for (unsigned j = 0; j < 4; ++j) key[j] = be32(reinterpret_cast<const unsigned char *>(data.data() + offset + j * 4));
        found = true; break;
    }
    if (!found) throw std::runtime_error("Original ELF credential key mapping is missing");
    unsigned char padded[24] = {}; std::memcpy(padded, password.data(), password.size());
    std::string result;
    for (unsigned i = 0; i < 24; i += 8) {
        uint32_t left = be32(padded + i), right = be32(padded + i + 4), total = 0;
        for (unsigned round = 0; round < 32; ++round) {
            left += (((right << 4) ^ (right >> 5)) + right) ^ (total + key[total & 3]);
            total += 0x9e3779b9;
            right += (((left << 4) ^ (left >> 5)) + left) ^ (total + key[(total >> 11) & 3]);
        }
        for (uint32_t word : {left, right}) for (int shift = 28; shift >= 0; shift -= 4) result += "0123456789abcdef"[(word >> shift) & 15];
    }
    return result;
}

static const wchar_t *application_schemas[] = {
    L"d_channel",L"d_guild",L"d_taiwan",L"d_taiwan_secu",L"d_technical_report",L"taiwan_billing",L"taiwan_cain",
    L"taiwan_cain_2nd",L"taiwan_cain_auction_cera",L"taiwan_cain_auction_gold",L"taiwan_cain_log",L"taiwan_cain_web",
    L"taiwan_game_event",L"taiwan_login",L"taiwan_login_play",L"taiwan_mng_manager",L"taiwan_prod",L"taiwan_se_event"
};

static ToolResult sql_input(const std::wstring &exe, const std::wstring &instance, const std::wstring &path) {
    return run_tool(exe, instance, {L"mysql",instance,L"--defaults-file=client.private.cnf",L"--comments",L"--binary-mode",
                    L"--default-character-set=latin1",L"--batch",L"--skip-column-names"}, 180000, path);
}

static void checked_sql_file(const std::wstring &exe, const std::wstring &instance, const std::wstring &path) {
    ToolResult result = sql_input(exe, instance, path);
    if (result.code) throw std::runtime_error("Application database import failed; private SQL log retained");
}

static void create_game_instance(HWND window, const std::wstring &exe, const std::wstring &resources,
                                 const std::wstring &instance, Session &live) {
    std::wstring manifest = resources + L"\\resources.ini";
    std::wstring version = setting(manifest, L"resources", L"version");
    bool cabinet = version == L"2";
    if (version != L"1" && !cabinet) throw std::runtime_error("Local game resources are missing or incompatible");
    unsigned count = GetPrivateProfileIntW(L"resources", L"count", 0, manifest.c_str());
    if (!count) throw std::runtime_error("Local game resource list is empty");
    std::vector<std::pair<std::wstring, std::string>> files;
    PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在核对本地游戏资源…"));
    for (unsigned i = 0; i < count; ++i) {
        std::wstring section = L"file" + std::to_wstring(i);
        auto path = resource_path(setting(manifest, section, L"path"));
        std::string hash = ascii(setting(manifest, section, L"sha256"));
        if (!cabinet && file_sha256(resources + L"\\" + path) != hash) throw std::runtime_error("Local game resource hash differs; instance was not created");
        files.emplace_back(path, hash);
    }
    std::string seed_hash = ascii(setting(manifest, L"resources", L"seed_sha256"));
    if (cabinet) {
        if (file_sha256(resources + L"\\resources.cab") != ascii(setting(manifest, L"resources", L"cabinet_sha256")))
            throw std::runtime_error("Local game cabinet hash differs; instance was not created");
    } else if (file_sha256(resources + L"\\seed.sql") != seed_hash) throw std::runtime_error("Application seed identity differs");
    if (present(instance)) {
        if (!(GetFileAttributesW(instance.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("Instance path must be a directory; existing file was not changed");
        WIN32_FIND_DATAW item; HANDLE search = FindFirstFileW((instance + L"\\*").c_str(), &item);
        if (search != INVALID_HANDLE_VALUE) {
            bool occupied = false;
            do { if (std::wcscmp(item.cFileName, L".") && std::wcscmp(item.cFileName, L"..")) { occupied = true; break; } }
            while (FindNextFileW(search, &item));
            FindClose(search);
            if (occupied) throw std::runtime_error("Create game instance requires an empty directory; existing files were not changed");
        }
    } else directory(instance);
    PSECURITY_DESCRIPTOR security = private_security();
    bool secured = SetFileSecurityW(instance.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, security);
    LocalFree(security); require(secured, "Protect game instance directory");
    write_private(instance + L"\\instance.pending", "Native game instance creation has not completed. Private diagnostics retained.\n");
    PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在复制游戏资源并安全初始化数据库…"));
    std::wstring seed = resources + L"\\seed.sql", service_template = resources + L"\\services.template.ini";
    if (cabinet) {
        directory(instance + L"\\.mysql");
        seed = instance + L"\\.mysql\\setup-seed.sql";
        service_template = instance + L"\\.mysql\\setup-services.ini";
        auto target = [&](const std::wstring &path) {
            return path == L"seed.sql" ? seed : path == L"services.template.ini" ? service_template : instance + L"\\" + path;
        };
        std::map<std::string, std::wstring> objects;
        for (const auto &[path, hash] : files) objects.emplace(hash, target(path));
        PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在解压并核对本地游戏资源…"));
        extract_resource_cabinet(resources + L"\\resources.cab", objects);
        for (const auto &[path, hash] : files) {
            std::wstring destination = target(path);
            if (objects.at(hash) != destination) {
                directory(destination.substr(0, destination.find_last_of(L'\\')));
                require(CopyFileW(objects.at(hash).c_str(), destination.c_str(), TRUE), "Copy identical resource to original path");
            }
            if (file_sha256(destination) != hash) throw std::runtime_error("Extracted game resource identity differs; incomplete instance retained");
        }
        if (file_sha256(seed) != seed_hash) throw std::runtime_error("Extracted application seed identity differs");
    } else for (const auto &[path, hash] : files) {
        if (path == L"seed.sql" || path == L"services.template.ini") continue;
        std::wstring target = instance + L"\\" + path;
        directory(target.substr(0, target.find_last_of(L'\\')));
        require(CopyFileW((resources + L"\\" + path).c_str(), target.c_str(), TRUE), "Copy local game resource");
        if (file_sha256(target) != hash) throw std::runtime_error("Copied game resource identity differs; incomplete instance retained");
    }
    for (const wchar_t *path : {L"i686\\tmp",L"i686\\dev\\shm\\sec",L"service-logs"}) directory(instance + L"\\" + path);
    // Directory-only runtime paths are absent from a file manifest.
    const wchar_t *names[] = {L"bridge",L"manager",L"dbmw_guild",L"dbmw_mnt",L"dbmw_stat",L"monitor",L"guild",L"statics",L"coserver",L"community",L"auction",L"point",L"channel",L"relay",L"game"};
    for (const wchar_t *name : names) for (const wchar_t *dir : {L"log",L"pid",L"history"})
        directory(instance + L"\\i686\\home\\neople\\" + name + L"\\" + dir);
    for (const wchar_t *name : {L"gunnersvr",L"zergsvr"}) for (const wchar_t *dir : {L"log",L"pid",L"history"})
        directory(instance + L"\\i686\\home\\neople\\secsvr\\" + name + L"\\" + dir);
    for (const wchar_t *dir : {L"log",L"pid"}) directory(instance + L"\\x86_64\\home\\neople\\stun\\" + dir);
    ToolResult init = run_tool(exe, instance, {L"initialize",instance,L"3306"});
    if (init.code) throw std::runtime_error("Secure game database initialization failed; private log retained");
    live = start_database(window, exe, instance);
    PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在导入并逐字节核对应用数据库…"));
    checked_sql_file(exe, instance, seed);
    std::vector<std::wstring> dump{L"mysqldump",instance,L"--defaults-file=client.private.cnf",L"--default-character-set=latin1",L"--lock-all-tables",
        L"--routines",L"--events",L"--triggers",L"--hex-blob",L"--skip-comments",L"--set-gtid-purged=OFF",L"--databases"};
    dump.insert(dump.end(), std::begin(application_schemas), std::end(application_schemas));
    ToolResult exported = run_tool(exe, instance, dump, 180000);
    if (exported.code || file_sha256(exported.log) != seed_hash)
        throw std::runtime_error("Imported application database does not re-export byte for byte; incomplete instance retained");
    std::string game = instance_secret(8), bridge = instance_secret(16), frida = instance_secret(16);
    std::string encoded = legacy_password(instance + L"\\i686\\home\\neople\\dbmw_guild\\df_dbmw_r", game);
    std::string query = "CREATE USER 'dnf_game'@'127.0.0.1' IDENTIFIED BY '" + game + "';";
    // Keep permissions at the application's existing schema boundary. This is
    // independent from the generated private root maintenance credential.
    for (const wchar_t *schema : application_schemas) query += "GRANT ALL ON `" + ascii(schema) + "`.* TO 'dnf_game'@'127.0.0.1';";
    query += "CREATE USER 'dnf_bridge'@'127.0.0.1' IDENTIFIED BY '" + bridge + "';GRANT SELECT ON d_channel.* TO 'dnf_bridge'@'127.0.0.1';"
             "CREATE USER 'dnf_frida'@'127.0.0.1' IDENTIFIED BY '" + frida + "';CREATE DATABASE frida;"
             "GRANT ALL ON d_taiwan.* TO 'dnf_frida'@'127.0.0.1';GRANT ALL ON frida.* TO 'dnf_frida'@'127.0.0.1';"
             "UPDATE d_taiwan.db_connect SET db_ip='127.0.0.1',db_port=3306,db_userid='dnf_game',db_passwd='" + encoded + "';\n";
    std::wstring sql_path = instance + L"\\.mysql\\" + unique_name() + L"-accounts.private.sql";
    write_private(sql_path, query); checked_sql_file(exe, instance, sql_path);
    require(DeleteFileW(sql_path.c_str()), "Remove completed private account SQL");
    ToolResult date = sql(exe, instance, L"SELECT DATE_FORMAT(NOW(),'%Y%m');");
    std::string month = date.output.substr(0,6);
    if (date.code || month.size() != 6 || month.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Read application monthly table date failed");
    std::wstring monthly;
    for (const wchar_t *schema : {L"taiwan_cain_auction_gold",L"taiwan_cain_auction_cera"}) for (const wchar_t *table : {L"auction_history",L"auction_history_buyer"})
        monthly += L"CREATE TABLE IF NOT EXISTS " + std::wstring(schema) + L"." + table + L"_" + decode(month) + L" LIKE " + schema + L"." + table + L";";
    if (sql(exe, instance, monthly.c_str()).code) throw std::runtime_error("Prepare monthly application tables failed");
    for (const wchar_t *name : {L"dbmw_guild\\cfg\\dbmw_gld_siroco.cfg",L"dbmw_mnt\\cfg\\dbmw_mnt_siroco.cfg",L"dbmw_stat\\cfg\\dbmw_stat_siroco.cfg",
                               L"game\\cfg\\siroco11.cfg",L"auction\\cfg\\auction_siroco.cfg",L"point\\cfg\\point_siroco.cfg"}) {
        std::wstring path = instance + L"\\i686\\home\\neople\\" + name;
        std::string text = read_file(path); replace_all(text, "@DNF_GAME_PASSWORD@", encoded); write_private(path, text, CREATE_ALWAYS);
    }
    for (const auto &entry : std::vector<std::pair<std::wstring,std::pair<std::string,std::string>>>{
            {L"i686\\home\\neople\\bridge\\cfg\\bridge.cfg", {"@DNF_BRIDGE_PASSWORD@",bridge}},
            {L"i686\\dp2\\df_game_r.js", {"@DNF_FRIDA_PASSWORD@",frida}}}) {
        std::wstring path = instance + L"\\" + entry.first;
        std::string text = read_file(path); replace_all(text,entry.second.first,entry.second.second); write_private(path,text,CREATE_ALWAYS);
    }
    stop_database(window, exe, instance, live); CloseHandle(live.process); live = {};
    require(CopyFileW(service_template.c_str(), (instance + L"\\services.ini").c_str(), TRUE), "Commit instance service list");
    if (cabinet) {
        require(DeleteFileW(seed.c_str()), "Remove imported setup seed");
        require(DeleteFileW(service_template.c_str()), "Remove setup service template");
    }
    write_private(instance + L"\\instance-setup.txt", "Application seed SHA256=" + seed_hash + "\nImport re-export: byte identical.\nApplication accounts: generated per instance.\nDatabase stopped normally.\n");
    require(DeleteFileW((instance + L"\\instance.pending").c_str()), "Commit completed game instance");
}
