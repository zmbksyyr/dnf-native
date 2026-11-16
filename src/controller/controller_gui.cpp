// Independent Win32 console for original ELF hosts and the official MySQL tools.
#include "service_processes.h"
#include <ws2tcpip.h>
#include <uxtheme.h>
#include <commctrl.h>
#include "log_view.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <thread>

enum { INSTANCE = 101, BROWSE, PORT, INITIALIZE = 110, START, STOP, BACKUP, RESTORE, RECOVER, FOLDER, LOGS,
       SERVICE_LIST = 220, ELF_START, ELF_STOP, ALL_START, ALL_STOP, SERVICE_LOGS,
       DP2_ENABLE = 230, FRIDA_ENABLE, PLUGINS_SAVE,
       LOG_SOURCE = 240, LOG_ENCODING, LOG_FOLLOW, LOG_REFRESH, LOG_FOLDER,
       SERVICE_IP = 250, IP_APPLY, CREATE_INSTANCE = 260, DELETE_INSTANCE, CHECK_RESOURCES,
       SERVICE_DETAIL = 219, SERVICE_META = 272, SERVICE_READINESS, MAINTENANCE_DETAIL };
enum { INSTANCE_MORE = 280, SERVICE_INFO, DATABASE_INFO, LOG_FILTER,
       LOG_CONSOLE_TAB, LOG_SERVICE_TAB, LOG_ERROR_TAB, LOG_MYSQL_TAB, LOG_FILES };
enum { STATUS_MESSAGE = WM_APP + 1, DONE_MESSAGE, PROGRESS_MESSAGE };

static bool present(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring full_path(const std::wstring &value) {
    return instance_directory(value);
}

#include "service_ip.h"
#include "instance_files.h"

static std::wstring decode(const std::string &bytes) {
    if (bytes.empty()) return {};
    UINT codepage = CP_UTF8;
    int size = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!size) { codepage = CP_ACP; size = MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0); }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()), result.data(), size);
    return result;
}

static std::wstring window_text(HWND window) {
    std::wstring text(GetWindowTextLengthW(window) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}

static std::wstring unique_name() {
    static unsigned sequence = 0; // Only the single operation worker calls this.
    return L"gui-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
}

struct ToolResult { DWORD code; std::wstring log; std::string output; };

static HANDLE launch_tool(const std::wstring &exe, const std::wstring &instance,
                          const std::vector<std::wstring> &arguments, const std::wstring &log,
                          const std::wstring &input_path = L"") {
    directory(instance + L"\\.mysql");
    File output = private_file(log);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    File input(CreateFileW(input_path.empty() ? L"NUL" : input_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, &security, OPEN_EXISTING, 0, nullptr));
    std::wstring command = quote(exe);
    for (const auto &argument : arguments) command += L" " + quote(argument);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.handle;
    startup.hStdOutput = startup.hStdError = output.handle;
    PROCESS_INFORMATION process = {};
    require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, instance.c_str(), &startup, &process), "Start native database operation");
    CloseHandle(process.hThread);
    return process.hProcess;
}

static DWORD exit_code(HANDLE process) {
    DWORD code;
    require(GetExitCodeProcess(process, &code), "Read database process result");
    return code;
}

static ToolResult run_tool(const std::wstring &exe, const std::wstring &instance,
                           const std::vector<std::wstring> &arguments, DWORD timeout = INFINITE,
                           const std::wstring &input_path = L"") {
    std::wstring log = instance + L"\\.mysql\\" + unique_name() + L".private.log";
    HANDLE process = launch_tool(exe, instance, arguments, log, input_path);
    DWORD waited = WaitForSingleObject(process, timeout);
    if (waited != WAIT_OBJECT_0) {
        // This wrapper owns its official child through the controller's Job. This
        // timeout concerns a metadata client, never an unrelated MySQL server.
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 10000);
        CloseHandle(process);
        throw std::runtime_error("Database metadata command timed out; private log retained");
    }
    DWORD code = exit_code(process);
    CloseHandle(process);
    return {code, log, read_file(log)};
}

struct Session { HANDLE process = nullptr; std::wstring log, pid_name; std::string uuid; };

static ToolResult sql(const std::wstring &exe, const std::wstring &instance, const wchar_t *query) {
    return run_tool(exe, instance, {L"mysql", instance, L"--defaults-file=client.private.cnf", L"--connect-timeout=5",
                    L"--batch", L"--skip-column-names", L"--execute=" + std::wstring(query)}, 15000);
}

static bool owns_response(const ToolResult &result, const Session &session) {
    std::string expected = "5.7.44\t" + session.uuid + "\t" + std::string(session.pid_name.begin(), session.pid_name.end());
    return !result.code && result.output.find(expected + "\r\n") != std::string::npos &&
           result.output.find("Ssl_cipher\t\r\n") == std::string::npos && result.output.find("Ssl_cipher\t") != std::string::npos;
}

static const wchar_t *identity_query = L"SELECT VERSION(),@@server_uuid,SUBSTRING_INDEX(REPLACE(@@pid_file,CHAR(92),'/'),'/',-1); SHOW SESSION STATUS LIKE 'Ssl_cipher';";

static Session start_database(HWND window, const std::wstring &exe, const std::wstring &instance) {
    Session session;
    std::string config = read_file(instance + L"\\data\\auto.cnf");
    size_t start = config.find("server-uuid=");
    if (start == std::string::npos) throw std::runtime_error("Instance server UUID is missing");
    session.uuid = config.substr(start + 12, 36);
    session.pid_name = unique_name() + L".pid";
    session.log = instance + L"\\.mysql\\" + unique_name() + L"-server.private.log";
    session.process = launch_tool(exe, instance, {L"mysqld", instance, L"--defaults-file=server.cnf", L"--console",
                                  L"--pid-file=" + instance + L"\\.mysql\\" + session.pid_name}, session.log);
    PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在启动数据库并核对 SQL 身份…"));
    try {
        ULONGLONG deadline = GetTickCount64() + 45000;
        for (;;) {
            if (WaitForSingleObject(session.process, 0) != WAIT_TIMEOUT)
                throw std::runtime_error("Owned database exited during startup; inspect its private server log");
            ToolResult check = sql(exe, instance, identity_query);
            if (owns_response(check, session)) return session;
            if (!check.code) throw std::runtime_error("SQL response belongs to a different instance or lacks TLS; owned startup aborted");
            if (GetTickCount64() >= deadline) throw std::runtime_error("Database SQL readiness timed out; inspect private logs");
            Sleep(100);
        }
    } catch (...) {
        if (WaitForSingleObject(session.process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(session.process, 1);
            WaitForSingleObject(session.process, 10000);
        }
        CloseHandle(session.process);
        throw;
    }
}

static void stop_database(HWND window, const std::wstring &exe, const std::wstring &instance, const Session &session) {
    PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在正常停止数据库…"));
    if (WaitForSingleObject(session.process, 0) == WAIT_TIMEOUT) {
        if (!owns_response(sql(exe, instance, identity_query), session))
            throw std::runtime_error("Database identity check failed; no shutdown command sent");
        ToolResult stop = sql(exe, instance, L"SHUTDOWN;");
        if (WaitForSingleObject(session.process, 60000) != WAIT_OBJECT_0)
            throw std::runtime_error("Owned database has not exited; it was not force-stopped");
        bool lost_ack = stop.code == 1 && stop.output.find("ERROR 2013 (HY000)") != std::string::npos;
        if (stop.code && !lost_ack) throw std::runtime_error("Shutdown client failed; private log retained");
    }
    if (exit_code(session.process) || read_file(session.log).find("Shutdown complete") == std::string::npos)
        throw std::runtime_error("Database did not record a normal complete shutdown");
}

#include "instance_setup.h"

struct Result {
    bool success = false, stopped = false, created = false, deleted = false;
    Session started;
    std::wstring status, details, maintenance;
};

static std::wstring service_group(const std::wstring &name) {
    if (name == L"monitor" || name == L"manager" || name == L"dbmw_guild" || name == L"dbmw_mnt" ||
        name == L"dbmw_stat" || name == L"guild" || name == L"statics" || name == L"coserver") return L"核心";
    if (name == L"bridge" || name == L"channel" || name == L"relay" || name == L"community") return L"辅助";
    if (name == L"game" || name == L"auction" || name == L"point") return L"游戏/插件";
    if (name == L"stun" || name == L"secagent" || name == L"gunnersvr" || name == L"zergsvr") return L"安全";
    return L"其他";
}

static std::wstring service_readiness(const Service &service) {
    if (!service.readiness.empty()) return service.readiness;
    return service.state == L"未启动" ? L"未启动" : L"未验证";
}

struct App {
    HWND window = nullptr, path = nullptr, port = nullptr, service_ip = nullptr, status = nullptr, output = nullptr;
    HFONT font = nullptr, strong_font = nullptr, log_font = nullptr, small_font = nullptr;
    HBRUSH background = nullptr, surface = nullptr;
    std::wstring exe, initial, current, resources, runtime;
    Session session;
    std::vector<Service> services;
    std::vector<std::wstring> recent_instances;
    std::wstring preferred_service;
    HANDLE service_job = nullptr, instance_lock = nullptr;
    bool busy = false, closing = false;
    int operation = 0;
    std::thread worker;
    std::vector<LogSource> log_sources;
    std::wstring operations, shown_log;
    ULONGLONG log_size = 0, log_modified = 0, log_scan = 0;
    bool log_loaded = false;
    std::wstring maintenance_summary = L"尚未执行";
    int dpi = 96;
    int px(int value) const { return MulDiv(value, dpi, 96); }
    bool resources_ready = false;
    void check_resources() {
        std::wstring missing = missing_resources(resources);
        resources_ready = missing.empty();
        SetWindowTextW(GetDlgItem(window, 218), resources_ready ? L"资源齐全：MySQL · x86/x64 host · 原生服务端 ZIP" : (L"缺少资源：" + missing).c_str());
    }
    void close_instance_lease() {
        if (instance_lock) { CloseHandle(instance_lock); instance_lock = nullptr; }
    }


    void display_log(const std::wstring &text) {
        std::wstring filtered;
        std::wstring needle = window_text(GetDlgItem(window, LOG_FILTER));
        if (!needle.empty()) {
            std::transform(needle.begin(), needle.end(), needle.begin(), std::towlower);
            size_t begin = 0;
            while (begin < text.size()) {
                size_t end = text.find(L'\n', begin);
                if (end == std::wstring::npos) end = text.size(); else ++end;
                std::wstring line = text.substr(begin, end - begin), lower = line;
                std::transform(lower.begin(), lower.end(), lower.begin(), std::towlower);
                if (lower.find(needle) != std::wstring::npos) filtered += line;
                begin = end;
            }
        }
        const std::wstring &shown = needle.empty() ? text : filtered;
        bool follow = SendMessageW(GetDlgItem(window, LOG_FOLLOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
        DWORD first = 0, last = 0;
        SendMessageW(output, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        int line = static_cast<int>(SendMessageW(output, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(output, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(output, shown.c_str());
        if (follow) SendMessageW(output, EM_SETSEL, shown.size(), shown.size());
        else {
            SendMessageW(output, EM_SETSEL, std::min<size_t>(first, shown.size()), std::min<size_t>(last, shown.size()));
        }
        SendMessageW(output, WM_SETREDRAW, TRUE, 0);
        if (follow) SendMessageW(output, EM_SCROLLCARET, 0, 0);
        else SendMessageW(output, EM_LINESCROLL, 0, line);
        InvalidateRect(output, nullptr, TRUE);
        for (int id : {LOG_CONSOLE_TAB,LOG_SERVICE_TAB,LOG_ERROR_TAB,LOG_MYSQL_TAB}) InvalidateRect(GetDlgItem(window,id),nullptr,FALSE);
    }
    void refresh_log(bool force = false) {
        LRESULT index = SendMessageW(GetDlgItem(window, LOG_SOURCE), CB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<size_t>(index) >= log_sources.size()) return;
        const auto &source = log_sources[index];
        if (source.path.empty()) {
            SetWindowTextW(GetDlgItem(window, 210), L"控制台操作记录");
            if (force || shown_log != L"console") { shown_log = L"console"; display_log(operations); }
            return;
        }
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(source.path.c_str(), GetFileExInfoStandard, &attributes)) {
            SetWindowTextW(GetDlgItem(window, 210), (source.path + L"  ·  文件暂不可读").c_str());
            return;
        }
        ULONGLONG size = (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        ULONGLONG modified = log_time(attributes.ftLastWriteTime);
        if (!force && log_loaded && shown_log == source.path && size == log_size && modified == log_modified) return;
        try {
            LogTail tail = read_log_tail(source.path);
            unsigned encoding = static_cast<unsigned>(SendMessageW(GetDlgItem(window, LOG_ENCODING), CB_GETCURSEL, 0, 0));
            display_log(log_text(tail.bytes, encoding));
            shown_log = source.path; log_size = tail.size; log_modified = tail.modified; log_loaded = true;
            SetWindowTextW(GetDlgItem(window, 210), (source.path + (tail.truncated ? L"  ·  显示末尾 256 KiB" : L"")).c_str());
        } catch (const std::exception &error) {
            SetWindowTextW(GetDlgItem(window, 210), (source.path + L"  ·  " + decode(error.what())).c_str());
        }
    }
    void scan_logs(bool reset = false) {
        if (busy) return; // The operation worker owns mutable service/session fields.
        if (log_sources.size() == 1 && log_sources[0].path.empty()) reset = true;
        std::wstring previous;
        LRESULT old = SendMessageW(GetDlgItem(window, LOG_SOURCE), CB_GETCURSEL, 0, 0);
        if (!reset && old >= 0 && static_cast<size_t>(old) < log_sources.size()) previous = log_sources[old].path;
        std::vector<LogSource> sources;
        int selected = selected_service_index();
        if (selected >= 0 && static_cast<size_t>(selected) < services.size()) {
            const Service &service = services[static_cast<size_t>(selected)];
            application_logs(service.root + service.cwd + L"\\log", L"", sources);
            std::sort(sources.begin(), sources.end(), [](const LogSource &a, const LogSource &b) {
                if (a.preferred != b.preferred) return a.preferred;
                return a.modified != b.modified ? a.modified > b.modified : a.path < b.path;
            });
            std::wstring out = (busy || service.out.empty()) ? latest_log(current + L"\\service-logs", service.name + L"-*.stdout.private.log") : service.out;
            std::wstring err = (busy || service.err.empty()) ? latest_log(current + L"\\service-logs", service.name + L"-*.stderr.private.log") : service.err;
            if (!out.empty()) sources.push_back({L"标准输出 · " + service.name, out});
            if (!err.empty()) sources.push_back({L"标准错误 / 宿主诊断 · " + service.name, err});
        }
        std::wstring mysql = session.log.empty() ? latest_log(current + L"\\.mysql", L"gui-*-server.private.log") : session.log;
        if (!mysql.empty()) sources.push_back({L"MySQL 原生日志", mysql});
        sources.push_back({L"控制台操作", L""});
        // Keep a stable source order while the user is reading. New/rotated files are still discovered.
        if (!reset && sources.size() == log_sources.size()) {
            bool same = std::all_of(sources.begin(), sources.end(), [&](const LogSource &source) {
                return std::any_of(log_sources.begin(), log_sources.end(), [&](const LogSource &old_source) { return old_source.path == source.path; });
            });
            if (same) return;
        }
        log_sources = std::move(sources);
        HWND combo = GetDlgItem(window, LOG_SOURCE);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int chosen = 0;
        for (size_t i = 0; i < log_sources.size(); ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(log_sources[i].label.c_str()));
            if (!reset && old >= 0 && log_sources[i].path == previous) chosen = static_cast<int>(i);
        }
        SendMessageW(combo, CB_SETCURSEL, chosen, 0);
        log_loaded = false; refresh_log(true);
    }

    bool services_running() const {
        return std::any_of(services.begin(), services.end(), [](const Service &service) { return running(service); });
    }
    Service *game_service() {
        auto found = std::find_if(services.begin(), services.end(), [](const Service &service) { return service.name == L"game"; });
        return found == services.end() ? nullptr : &*found;
    }
    void show_plugins() {
        const Service *game = game_service();
        bool dp2 = game && std::find(game->environment.begin(), game->environment.end(), dp2_preload) != game->environment.end();
        bool frida = dp2 && std::find(game->environment.begin(), game->environment.end(), L"DNF_NATIVE_FRIDA=0") == game->environment.end();
        SendMessageW(GetDlgItem(window, DP2_ENABLE), BM_SETCHECK, dp2 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(window, FRIDA_ENABLE), BM_SETCHECK, frida ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    void save_plugins() {
        Service *game = game_service();
        if (!game || running(*game)) throw std::runtime_error("Stop game before changing plugin settings");
        bool dp2 = SendMessageW(GetDlgItem(window, DP2_ENABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
        bool frida = dp2 && SendMessageW(GetDlgItem(window, FRIDA_ENABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (dp2 && !frida) {
            // Keep the original module and its documented -1000 load failure
            // result. The explicit off setting prevents Gadget loading while
            // allowing the default Lua entry script to continue running.
            const std::string guard = "\n    if os.getenv(\"DNF_NATIVE_FRIDA\") == \"0\" then\n"
                "        logger.info(\"Frida disabled by instance configuration\")\n        return -1000\n    end\n";
            std::wstring module = game->root + L"\\dp2\\lua\\df\\frida.lua";
            std::string source = read_file(module);
            if (source.find(guard) == std::string::npos) {
                const std::string entry = "function M.load(args, hproc)";
                size_t at = source.find(entry);
                if (at == std::string::npos || source.find(entry, at + entry.size()) != std::string::npos)
                    throw std::runtime_error("Frida module entry differs; plugin settings were not changed");
                require(CopyFileW(module.c_str(), (module + L".before-native-switch").c_str(), TRUE), "Preserve original instance Frida module");
                source.insert(at + entry.size(), guard);
                std::wstring temporary = module + L".native-tmp";
                write_private(temporary, source);
                require(MoveFileExW(temporary.c_str(), module.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Save instance Frida switch");
            }
        }
        const wchar_t *mode = !dp2 ? L"off" : frida ? L"frida" : L"lua";
        require(WritePrivateProfileStringW(L"plugins", L"mode", mode, (current + L"\\services.ini").c_str()), "Save game plugin mode");
        for (const auto &service : load_services(current, runtime)) if (service.name == L"game") game->environment = service.environment;
        show_plugins(); controls();
        SetWindowTextW(status, L"插件设置已保存，下次启动 game 生效");
        append(!dp2 ? L"DP2/Lua 已关闭，Frida 已关闭" : frida ? L"DP2/Lua 已启用，Frida 已启用" : L"DP2/Lua 已启用，Frida 已关闭");
    }
    int selected_service_index() const {
        HWND list = GetDlgItem(window, SERVICE_LIST);
        if (!list) return -1;
        LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
        return row >= 0 && static_cast<size_t>(row) < services.size() ? static_cast<int>(row) : -1;
    }
    std::wstring database_identity() const {
        std::string uuid = session.uuid;
        if (uuid.empty() && !current.empty() && present(current + L"\\data\\auto.cnf")) {
            std::string config = read_file(current + L"\\data\\auto.cnf");
            size_t at = config.find("server-uuid=");
            if (at != std::string::npos) uuid = config.substr(at + 12, 36);
        }
        if (uuid.empty()) return L"未初始化";
        return decode(uuid) + (session.process ? L"（TLS 与实例身份已核对）" : L"（数据库未启动）");
    }
    std::wstring stop_plan(bool compact = true) const {
        std::wstring plan;
        unsigned count = 0;
        for (auto item = services.rbegin(); item != services.rend(); ++item) {
            if (!running(*item)) continue;
            ++count;
            if (compact && count > 4) continue;
            if (!plan.empty()) plan += L" → ";
            plan += item->name;
        }
        if (plan.empty()) return L"当前没有运行中的服务";
        if (compact && count > 4) plan += L" → …（共 " + std::to_wstring(count) + L" 项）";
        return plan;
    }
    void show_maintenance_detail() {
        HWND detail = GetDlgItem(window, MAINTENANCE_DETAIL);
        if (!detail) return;
        std::wstring text = L"数据库：" + database_identity() + L"\r\n将按逆序停止：" + stop_plan();
        if (!maintenance_summary.empty()) text += L"\r\n最近维护：" + maintenance_summary;
        SetWindowTextW(detail, text.c_str());
        SetWindowTextW(GetDlgItem(window,212), (L"数据库  " + database_identity()).c_str());
    }
    void show_service_detail() {
        int selected = selected_service_index();
        if (selected < 0) {
            SetWindowTextW(GetDlgItem(window, SERVICE_DETAIL), L"请选择服务卡片");
            SetWindowTextW(GetDlgItem(window, SERVICE_META), L"");
            SetWindowTextW(GetDlgItem(window, SERVICE_READINESS), L"就绪层级：未选择");
            show_maintenance_detail();
            return;
        }
        const Service &service = services[static_cast<size_t>(selected)];
        SetWindowTextW(GetDlgItem(window,LOG_SERVICE_TAB),service.name.c_str());
        std::wstring architecture = service.host.find(L"x64") != std::wstring::npos ? L"x86_64" : L"i386";
        SetWindowTextW(GetDlgItem(window, SERVICE_DETAIL),
                       (service.name + L"  ·  " + service_group(service.name) + L"  ·  " + architecture).c_str());
        std::wstring recent = !service.err.empty() ? service.err : service.out;
        if (recent.empty()) recent = latest_log(service.root + service.cwd + L"\\log", L"*");
        std::wstring meta = L"根目录：" + service.root + L"\r\n工作目录：" + service.cwd +
                            L"\r\nELF：" + service.elf + L"\r\nSHA-256：" + service.elf_hash +
                            L"\r\n停服信号：" + std::to_wstring(service.signal) +
                            (service.automatic ? L" · 整体启动包含此项" : L" · 需手动启动");
        SetWindowTextW(GetDlgItem(window, SERVICE_META), meta.c_str());
        std::wstring readiness = L"就绪层级：" + service_readiness(service) +
                                 L"\r\n接口/协议/业务：未验证  ·  端口/依赖：清单未声明";
        if (!recent.empty()) readiness += L"\r\n最近日志：" + recent;
        SetWindowTextW(GetDlgItem(window, SERVICE_READINESS), readiness.c_str());
        show_maintenance_detail();
    }
    void show_services() {
        HWND list = GetDlgItem(window, SERVICE_LIST);
        int selected = selected_service_index();
        if (!preferred_service.empty()) {
            auto found = std::find_if(services.begin(), services.end(), [&](const Service &service) { return service.name == preferred_service; });
            if (found != services.end()) selected = static_cast<int>(found - services.begin());
        }
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        unsigned live = 0;
        for (const auto &service : services) {
            std::wstring item = service.name + (service.automatic ? L"" : L"（可选）") + L"  |  " + service.state;
            if (running(service)) { item += L" · PID " + std::to_wstring(service.pid); ++live; }
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }
        if (!services.empty()) SendMessageW(list, LB_SETCURSEL,
            selected >= 0 && static_cast<size_t>(selected) < services.size() ? selected : 0, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        std::wstring count = std::to_wstring(live) + L" / " + std::to_wstring(services.size()) + L" 运行中";
        SetWindowTextW(GetDlgItem(window, 213), count.c_str());
        InvalidateRect(GetDlgItem(window, BROWSE), nullptr, TRUE);
        show_service_detail();
        show_maintenance_detail();
    }

    void append(const std::wstring &text) {
        SYSTEMTIME now; GetLocalTime(&now);
        wchar_t stamp[24]; std::swprintf(stamp, 24, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
        std::wstring line = stamp + text + L"\r\n";
        operations += line;
        if (operations.size() > 256 * 1024) operations.erase(0, operations.find(L'\n', operations.size() - 256 * 1024) + 1);
        if (shown_log == L"console") display_log(operations);
    }
    void controls() {
        InvalidateRect(window,nullptr,FALSE);
        SetWindowTextW(GetDlgItem(window, 215), busy ? L"处理中…" : session.process ? L"运行中" : L"未启动");
        show_maintenance_detail();
        // The worker may be changing process ownership. Inspect it after join.
        if (busy) {
            for (int id : {INSTANCE,BROWSE,PORT,INITIALIZE,START,STOP,BACKUP,RESTORE,RECOVER,
                           CREATE_INSTANCE,DELETE_INSTANCE,CHECK_RESOURCES,SERVICE_IP,IP_APPLY,ELF_START,ELF_STOP,ALL_START,ALL_STOP,DP2_ENABLE,FRIDA_ENABLE,PLUGINS_SAVE})
                EnableWindow(GetDlgItem(window, id), FALSE);
            for (int id : {SERVICE_LIST,SERVICE_LOGS}) EnableWindow(GetDlgItem(window,id), FALSE);
            for (int id : {FOLDER,LOGS}) EnableWindow(GetDlgItem(window,id), !current.empty());
            SetWindowTextW(GetDlgItem(window,ALL_START),operation==ALL_START ? L"正在启动…" : L"整体启动");
            SetWindowTextW(GetDlgItem(window,ALL_STOP),operation==ALL_STOP ? L"正在停止…" : L"整体停止");
            return;
        }
        SetWindowTextW(GetDlgItem(window,ALL_START),L"整体启动");
        SetWindowTextW(GetDlgItem(window,ALL_STOP),L"整体停止");
        bool active = services_running();
        EnableWindow(GetDlgItem(window, CREATE_INSTANCE), resources_ready && !active && !session.process);
        EnableWindow(GetDlgItem(window, INSTANCE_MORE), !busy);
        EnableWindow(GetDlgItem(window, SERVICE_INFO), !services.empty());
        EnableWindow(GetDlgItem(window, DELETE_INSTANCE), !current.empty() && known_instance(current) && !active && !session.process);
        EnableWindow(GetDlgItem(window, CHECK_RESOURCES), TRUE);
        bool initialized = !current.empty() && present(current + L"\\data\\auto.cnf");
        bool restore_pending = !current.empty() && present(current + L"\\restore.pending");
        bool pending = restore_pending || (!current.empty() && present(current + L"\\instance.pending"));
        for (int id : {INSTANCE, BROWSE, PORT}) EnableWindow(GetDlgItem(window, id), !active && !session.process && (id != PORT || !initialized));
        EnableWindow(GetDlgItem(window, INITIALIZE), !busy && !session.process && !current.empty() && !initialized && !pending);
        EnableWindow(GetDlgItem(window, START), !busy && !session.process && initialized && !pending);
        EnableWindow(GetDlgItem(window, STOP), session.process && !active);
        // Maintenance owns the same stop path as the window close action.  It
        // is safe to offer it while the instance is running because the worker
        // first stops the original services in reverse order and then the
        // database before invoking the offline operation.
        for (int id : {BACKUP, RESTORE}) EnableWindow(GetDlgItem(window, id), initialized && !pending);
        EnableWindow(GetDlgItem(window, RECOVER), !busy && !active && !session.process && restore_pending);
        for (int id : {FOLDER, LOGS}) EnableWindow(GetDlgItem(window, id), !busy && !current.empty());
        EnableWindow(GetDlgItem(window, SERVICE_IP), initialized && !active && !session.process && !pending);
        EnableWindow(GetDlgItem(window, IP_APPLY), initialized && !active && !session.process && !pending);
        int index = selected_service_index();
        bool selected = index >= 0 && static_cast<size_t>(index) < services.size();
        EnableWindow(GetDlgItem(window, SERVICE_LIST), !services.empty());
        EnableWindow(GetDlgItem(window, ELF_START), selected && session.process && !running(services[static_cast<size_t>(index)]) && !pending);
        EnableWindow(GetDlgItem(window, ELF_STOP), selected && running(services[static_cast<size_t>(index)]));
        EnableWindow(GetDlgItem(window, SERVICE_LOGS), selected);
        EnableWindow(GetDlgItem(window, ALL_START), !services.empty() && initialized && !pending);
        EnableWindow(GetDlgItem(window, ALL_STOP), active || session.process);
        const Service *game = game_service();
        bool editable = game && !running(*game) && !pending;
        EnableWindow(GetDlgItem(window, DP2_ENABLE), editable);
        EnableWindow(GetDlgItem(window, FRIDA_ENABLE), editable && SendMessageW(GetDlgItem(window, DP2_ENABLE), BM_GETCHECK, 0, 0) == BST_CHECKED);
        EnableWindow(GetDlgItem(window, PLUGINS_SAVE), editable);
    }
    void load() {
        if (busy || session.process || services_running()) return;
        std::wstring value = window_text(path);
        if (value.empty()) {
            close_instance_lease(); current.clear(); services.clear(); maintenance_summary = L"尚未执行";
            show_services(); show_plugins(); scan_logs(true); SetWindowTextW(status, L"请选择实例目录"); controls(); return;
        }
        std::wstring chosen = full_path(value);
        if (_wcsicmp(chosen.c_str(), current.c_str())) {
            HANDLE next_lock = nullptr;
            try { if (known_instance(chosen)) next_lock = lease_instance(chosen); }
            catch (const std::exception &) {
                SetWindowTextW(path, current.c_str());
                SetWindowTextW(status, L"该实例正在另一个窗口中使用，请先关闭该窗口");
                controls(); return;
            }
            close_instance_lease(); instance_lock = next_lock;
        }
        current = chosen;
        if (known_instance(current) && std::find(recent_instances.begin(),recent_instances.end(),current)==recent_instances.end())
            recent_instances.push_back(current);
        maintenance_summary = L"尚未执行";
        services = load_services(current, runtime); show_services(); show_plugins(); scan_logs(true);
        SetWindowTextW(service_ip, configured_service_ip(services).c_str());
        SetWindowTextW(GetDlgItem(window, INITIALIZE), present(resources + L"\\resources.ini") ? L"创建实例" : L"初始化");
        bool initialized = present(current + L"\\data\\auto.cnf");
        if (present(current + L"\\server.cnf")) {
            std::string config = read_file(current + L"\\server.cnf");
            size_t at = config.find("\nport=");
            if (at != std::string::npos) SetWindowTextW(port, decode(config.substr(at + 6, config.find_first_of("\r\n", at + 6) - at - 6)).c_str());
        }
        SetWindowTextW(status, present(current + L"\\instance.pending") ? L"实例创建未完成，请查看日志并选择新的空目录" : present(current + L"\\restore.pending") ? L"需要恢复未完成的还原" : initialized ? L"已选择实例，可启动数据库" : L"尚未初始化数据库");
        controls();
    }
    void action(int id, const std::wstring &target = L"", int selected = -1) {
        current = full_path(window_text(path));
        const std::wstring instance = current, tool = exe, port_text = window_text(port), assets = resources;
        const Session owned = session;
        if (id == BACKUP || id == RESTORE || id == RECOVER) {
            maintenance_summary = (id == BACKUP ? L"备份" : id == RESTORE ? L"还原" : L"恢复") +
                                  std::wstring(L"准备中") + (target.empty() ? L"" : L" · " + target);
            append(L"维护计划：" + maintenance_summary + L"\r\n数据库：" + database_identity() +
                   L"\r\n逆序停服：" + stop_plan(false));
        }
        operation = id; busy = true; controls();
        SetWindowTextW(status, L"正在执行操作…");
        worker = std::thread([this, id, instance, tool, target, port_text, owned, selected, assets] {
            auto result = std::make_unique<Result>();
            auto stop_owned_services = [&] {
                for (auto item = services.rbegin(); item != services.rend(); ++item) {
                    if (running(*item)) {
                        auto message = new std::wstring(L"正在停止服务：" + item->name);
                        PostMessageW(window, PROGRESS_MESSAGE, 0, reinterpret_cast<LPARAM>(message));
                    }
                    stop_service(*item);
                }
            };
            try {
                if (id == DELETE_INSTANCE) {
                    if (!known_instance(instance) || instance.size() <= 3 ||
                        (GetFileAttributesW(instance.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
                        throw std::runtime_error("Select a real instance directory before deleting");
                    close_instance_lease();
                    remove_instance_tree(instance);
                    result->deleted = true;
                    result->status = L"实例文件夹已整体删除";
                    result->details = result->status + L"：" + instance;
                } else if (id == CREATE_INSTANCE || (id == INITIALIZE && present(assets + L"\\resources.ini"))) {
                    if (port_text != L"3306") throw std::runtime_error("Original bridge requires database port 3306 for a game instance");
                    create_game_instance(window, tool, assets, instance, result->started, instance_lock);
                    result->created = true;
                    result->status = L"游戏实例创建完成，数据库已停止";
                    result->details = result->status + L"\r\n已核对原始应用库逐字节导出，服务账号分别生成。";
                } else if (id == IP_APPLY) {
                    unsigned changed = update_service_ip(services, instance, target);
                    result->status = L"服务 IP 已更新";
                    result->details = changed ? L"已更新 " + std::to_wstring(changed) + L" 个服务配置文件；原配置已备份，下次启动生效。" : L"服务配置已使用此 IP，无需修改。";
                } else if (id == ALL_START || id == ALL_STOP || id == ELF_START || id == ELF_STOP) {
                    if (id == ALL_START && !owned.process) result->started = start_database(window, tool, instance);
                    if (id == ALL_START || id == ALL_STOP) {
                        if (id == ALL_START) {
                            for (auto &service : services) if (service.automatic) {
                                auto message = new std::wstring(L"正在启动服务：" + service.name);
                                PostMessageW(window, PROGRESS_MESSAGE, 0, reinterpret_cast<LPARAM>(message));
                                start_service(service, instance, service_job);
                            }
                            result->status = L"已启动默认服务进程（业务未验证）";
                        } else {
                            stop_owned_services();
                            if (owned.process) { stop_database(window, tool, instance, owned); result->stopped = true; }
                            result->status = L"服务进程已退出，数据库已停止";
                        }
                    } else {
                        if (selected < 0 || static_cast<size_t>(selected) >= services.size()) throw std::runtime_error("Choose a service first");
                        if (id == ELF_START) start_service(services[selected], instance, service_job);
                        else stop_service(services[selected]);
                        result->status = services[selected].name + L"：" + services[selected].state;
                    }
                    result->details = result->status;
                } else if (id == START) {
                    result->started = start_database(window, tool, instance);
                    result->status = L"数据库 SQL 检查通过（实例身份及 TLS 已核对）";
                    result->details = result->status + L"\r\n服务器日志：" + result->started.log;
                } else if (id == STOP) {
                    stop_database(window, tool, instance, owned);
                    result->stopped = true;
                    result->status = L"数据库已正常停止";
                    result->details = result->status + L"\r\n" + decode(read_file(owned.log));
                } else {
                    std::wstring preparation;
                    if (id == BACKUP || id == RESTORE) {
                        bool active = services_running();
                        if (active && !owned.process)
                            throw std::runtime_error("Services are running without an owned database; maintenance was not started");
                        if (active) {
                            preparation = L"维护前已按逆序停止服务和数据库\r\n";
                            PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(L"正在停止服务和数据库以进行维护…"));
                            stop_owned_services();
                        }
                        if (owned.process) {
                            stop_database(window, tool, instance, owned);
                            result->stopped = true;
                            if (preparation.empty()) preparation = L"维护前已停止数据库\r\n";
                        }
                    }
                    const wchar_t *operation = id == INITIALIZE ? L"initialize" : id == BACKUP ? L"backup" : id == RESTORE ? L"restore" : L"recover";
                    if (id == INITIALIZE) {
                        size_t used; unsigned long value = std::stoul(port_text, &used);
                        if (used != port_text.size() || !value || value > 65535) throw std::runtime_error("Database port must be 1-65535");
                    }
                    PostMessageW(window, STATUS_MESSAGE, 0, reinterpret_cast<LPARAM>(id == INITIALIZE ? L"正在安全初始化数据库…" : id == BACKUP ? L"正在备份数据库…" : id == RESTORE ? L"正在预备份、验证并还原数据库…" : L"正在恢复未完成的还原…"));
                    std::vector<std::wstring> arguments{operation, instance};
                    if (id == INITIALIZE) arguments.push_back(port_text);
                    if (id == BACKUP || id == RESTORE) arguments.push_back(target);
                    ToolResult command = run_tool(tool, instance, arguments);
                    result->details = preparation + decode(command.output) + L"\r\n操作日志：" + command.log;
                    if (!target.empty()) result->details = L"备份目录：" + target + L"\r\n" + result->details;
                    if (command.code) throw std::runtime_error("Native database operation failed; inspect the operation log");
                    result->status = id == INITIALIZE ? L"安全初始化完成，数据库已停止" : id == BACKUP ? L"备份完成，服务与数据库已停止" : id == RESTORE ? L"还原已验证并提交，服务与数据库已停止" : L"恢复完成，数据库已停止";
                    if (id == BACKUP || id == RESTORE || id == RECOVER) {
                        result->maintenance = result->status;
                        if (!target.empty()) result->maintenance += L" · " + target;
                    }
                }
                result->success = true;
            } catch (const std::exception &error) {
                result->status = L"操作失败：" + decode(error.what());
                result->details += L"\r\n" + result->status;
                if (id == BACKUP || id == RESTORE || id == RECOVER) result->maintenance = result->status;
                result->stopped = owned.process && WaitForSingleObject(owned.process, 0) == WAIT_OBJECT_0;
            }
            PostMessageW(window, DONE_MESSAGE, 0, reinterpret_cast<LPARAM>(result.release()));
        });
    }
};

static App *app;

static void rounded(HDC dc, RECT area, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    SelectObject(dc, old_brush); SelectObject(dc, old_pen);
    DeleteObject(brush); DeleteObject(pen);
}

static void focus_ring(HDC dc, RECT area, int inset = 2) {
    InflateRect(&area, -inset, -inset);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(43,108,246));
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int radius = app ? app->px(10) : 10;
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(pen);
}

static void text_at(HDC dc, RECT area, const std::wstring &text, COLORREF color,
                    HFONT font, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color); SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &area, format | DT_NOPREFIX);
}

static void dot_at(HDC dc, int x, int y, int size, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, y, x + size, y + size);
    SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(brush);
}

static std::wstring list_text(HWND list, int index) {
    LRESULT length = SendMessageW(list, LB_GETTEXTLEN, index, 0);
    if (length < 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(list, LB_GETTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(length)); return text;
}

static RECT service_tile(HWND window, int index) {
    RECT client; GetClientRect(window, &client);
    int gap = app->px(10), width = (client.right - 6 * gap) / 7;
    int left = (index % 7) * (width + gap), top = app->px((index / 7) * 62);
    return {left, top, index % 7 == 6 ? client.right : left + width, top + app->px(52)};
}

// The native list owns strings, selection and accessibility. Its presentation
// and hit rectangles use the same row-major grid, including keyboard movement.
static LRESULT CALLBACK service_grid_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                         UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == LB_GETITEMRECT) {
        if (wparam >= static_cast<WPARAM>(SendMessageW(window, LB_GETCOUNT, 0, 0))) return LB_ERR;
        *reinterpret_cast<RECT *>(lparam) = service_tile(window, static_cast<int>(wparam)); return 0;
    }
    auto hit = [&](POINT point) {
        int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) { RECT area = service_tile(window, i); if (PtInRect(&area, point)) return i; }
        return -1;
    };
    auto select = [&](int index) {
        if (index < 0) return;
        SendMessageW(window, LB_SETCURSEL, index, 0); InvalidateRect(window, nullptr, FALSE);
        SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(SERVICE_LIST, LBN_SELCHANGE), reinterpret_cast<LPARAM>(window));
    };
    if (message == LB_ITEMFROMPOINT) {
        int index = hit({static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))});
        return index < 0 ? MAKELRESULT(0, 1) : MAKELRESULT(index, 0);
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        if (!IsWindowEnabled(window)) return 0;
        SetFocus(window);
        select(hit({static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))}));
        if (message == WM_LBUTTONDBLCLK)
            SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(SERVICE_LIST, LBN_DBLCLK), reinterpret_cast<LPARAM>(window));
        return 0;
    }
    if (message == WM_LBUTTONUP || message == WM_MOUSEWHEEL) return 0;
    if (message == WM_KEYDOWN) {
        int index = static_cast<int>(SendMessageW(window, LB_GETCURSEL, 0, 0));
        int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0)), target = index;
        if (wparam == VK_LEFT) --target;
        else if (wparam == VK_RIGHT) ++target;
        else if (wparam == VK_UP) target -= 7;
        else if (wparam == VK_DOWN) target += 7;
        else if (wparam == VK_HOME) target = 0;
        else if (wparam == VK_END) target = count - 1;
        else if (wparam == VK_RETURN) {
            SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(SERVICE_LIST, LBN_DBLCLK), reinterpret_cast<LPARAM>(window)); return 0;
        } else return DefSubclassProc(window, message, wparam, lparam);
        if (target >= 0 && target < count) select(target);
        return 0;
    }
    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | (wparam == VK_RETURN ? DLGC_WANTALLKEYS : 0);
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint = {}; HDC target = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
        RECT client; GetClientRect(window, &client);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L,client.right), std::max(1L,client.bottom));
        HGDIOBJ old = SelectObject(dc, bitmap); FillRect(dc, &client, app->surface);
        int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
        int selected = static_cast<int>(SendMessageW(window, LB_GETCURSEL, 0, 0));
        for (int i = 0; i < count; ++i) {
            std::wstring snapshot = list_text(window, i);
            size_t split = snapshot.find(L"  |  ");
            std::wstring name = snapshot.substr(0, split), detail = split == std::wstring::npos ? L"" : snapshot.substr(split + 5);
            size_t optional = name.find(L"\uff08"); if (optional != std::wstring::npos) name.resize(optional);
            bool live = detail.find(L"\u8fdb\u7a0b\u8fd0\u884c\u4e2d") != std::wstring::npos;
            size_t code = detail.find(L"代码 ");
            bool failed = detail.find(L"\u5931\u8d25") != std::wstring::npos || detail.find(L"\u8d85\u65f6") != std::wstring::npos ||
                          (code != std::wstring::npos && code + 3 < detail.size() && detail[code+3] != L'0');
            RECT area = service_tile(window, i);
            bool chosen = i == selected;
            rounded(dc, area, chosen ? RGB(241,246,255) : failed ? RGB(255,247,247) : live ? RGB(255,255,255) : RGB(249,250,253),
                    chosen ? RGB(181,205,255) : failed ? RGB(255,208,208) : RGB(230,236,246), app->px(10));
            int center = (area.top + area.bottom) / 2;
            dot_at(dc, area.left + app->px(11), center - app->px(3), app->px(6),
                   live ? RGB(24,173,94) : failed ? RGB(223,49,53) : RGB(194,204,219));
            RECT label = {area.left + app->px(25), area.top, area.right - app->px(live ? 40 : 10), area.bottom};
            text_at(dc, label, name, chosen ? RGB(37,91,239) : failed ? RGB(220,42,49) : live ? RGB(36,51,75) : RGB(130,147,172),
                    chosen ? app->strong_font : app->font);
            size_t pid = detail.find(L"PID ");
            if (pid != std::wstring::npos) {
                RECT number = {area.right-app->px(38),area.top,area.right-app->px(8),area.bottom};
                text_at(dc, number, detail.substr(pid+4), RGB(153,169,192), app->small_font, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            }
            if (chosen && GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
                focus_ring(dc, area, app->px(2));
        }
        if (!count) text_at(dc, client, L"\u9009\u62e9\u6216\u521b\u5efa\u5b9e\u4f8b\u540e\u663e\u793a\u670d\u52a1", RGB(145,160,183), app->font, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
        if (message == WM_PAINT) EndPaint(window,&paint);
        return 0;
    }
    LRESULT result = DefSubclassProc(window,message,wparam,lparam);
    if (message == LB_SETCURSEL || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        InvalidateRect(window,nullptr,FALSE);
    return result;
}

static void draw_item(const DRAWITEMSTRUCT &item) {
    HDC dc = item.hDC; RECT area = item.rcItem;
    bool pressed = (item.itemState & ODS_SELECTED) != 0, disabled = (item.itemState & ODS_DISABLED) != 0;
    bool hover = GetPropW(item.hwndItem,L"hover") != nullptr;
    COLORREF fill = RGB(255,255,255), ink = RGB(54,77,112), border = RGB(215,225,242);
    FillRect(dc, &area, app->surface);
    if (item.CtlType == ODT_COMBOBOX) {
        if (pressed) fill=RGB(240,245,255);
        FillRect(dc, &area, app->surface);
        if (pressed) rounded(dc,area,fill,fill,app->px(8));
        std::wstring value;
        if (item.itemID != static_cast<UINT>(-1)) {
            int length=static_cast<int>(SendMessageW(item.hwndItem,CB_GETLBTEXTLEN,item.itemID,0));
            if (length>=0) {value.assign(length+1,L'\0');SendMessageW(item.hwndItem,CB_GETLBTEXT,item.itemID,reinterpret_cast<LPARAM>(value.data()));value.resize(length);}
        }
        area.left+=app->px(10); text_at(dc,area,value,ink,app->font); return;
    }
    if (disabled) { fill=RGB(247,249,252); ink=RGB(181,192,211); border=RGB(233,237,246); }
    else if (item.CtlID==ALL_START) {fill=pressed?RGB(28,77,201):hover?RGB(57,112,255):RGB(43,96,239); ink=RGB(255,255,255);border=fill;}
    else if (item.CtlID==ALL_STOP) {ink=RGB(215,54,57);border=RGB(252,205,205);fill=pressed?RGB(255,235,235):RGB(255,255,255);}
    else if (pressed || hover) {fill=RGB(242,247,255);ink=RGB(37,91,239);border=RGB(181,205,255);}
    bool tab=item.CtlID>=LOG_CONSOLE_TAB && item.CtlID<=LOG_MYSQL_TAB;
    if (tab) {
        LRESULT index=SendMessageW(GetDlgItem(app->window,LOG_SOURCE),CB_GETCURSEL,0,0);
        bool active=false;
        if (index>=0 && static_cast<size_t>(index)<app->log_sources.size()) {
            const auto &source=app->log_sources[static_cast<size_t>(index)];
            active=item.CtlID==LOG_CONSOLE_TAB?source.path.empty():item.CtlID==LOG_MYSQL_TAB?source.label==L"MySQL \u539f\u751f\u65e5\u5fd7":
                   item.CtlID==LOG_ERROR_TAB?source.label.find(L"\u6807\u51c6\u9519\u8bef")==0:!source.path.empty()&&source.label!=L"MySQL \u539f\u751f\u65e5\u5fd7"&&source.label.find(L"\u6807\u51c6\u9519\u8bef")!=0;
        }
        text_at(dc,area,window_text(item.hwndItem),active?RGB(39,93,241):RGB(112,132,162),active?app->strong_font:app->font,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(active) {RECT line={area.left,area.bottom-app->px(2),area.right,area.bottom};HBRUSH brush=CreateSolidBrush(RGB(43,96,239));FillRect(dc,&line,brush);DeleteObject(brush);}
    } else if (item.CtlID==BROWSE) {
        InflateRect(&area,-1,-1); rounded(dc,area,RGB(251,252,255),border,app->px(12));
        dot_at(dc,area.left+app->px(12),area.top+app->px(13),app->px(8),app->current.empty()?RGB(194,204,219):RGB(24,173,94));
        std::wstring name=app->current.empty()?L"\u9009\u62e9\u5b9e\u4f8b":app->current.substr(app->current.find_last_of(L"\\/")+1);
        RECT label={area.left+app->px(30),area.top+app->px(4),area.right-app->px(24),area.top+app->px(25)};
        text_at(dc,label,name,RGB(29,43,63),app->strong_font);
        label.top=area.top+app->px(25);label.bottom=area.bottom-app->px(3);
        text_at(dc,label,window_text(GetDlgItem(app->window,213))+L" \u00b7 MySQL "+window_text(GetDlgItem(app->window,215)),RGB(139,157,184),app->small_font);
        label={area.right-app->px(20),area.top,area.right-app->px(6),area.bottom};text_at(dc,label,L"\u2304",RGB(139,157,184),app->font);
    } else {
        bool ghost=item.CtlID==CHECK_RESOURCES||item.CtlID==FOLDER||item.CtlID==INSTANCE_MORE||item.CtlID==RECOVER||item.CtlID==PLUGINS_SAVE||item.CtlID==DATABASE_INFO||item.CtlID==SERVICE_INFO||item.CtlID==LOG_REFRESH;
        if(ghost && !pressed && !hover) {fill=RGB(255,255,255);border=fill;}
        InflateRect(&area,-1,-1);rounded(dc,area,fill,border,app->px(8));
        std::wstring label=window_text(item.hwndItem);
        if(item.CtlID==ALL_START)label=L"\u25b6 "+label;
        if(item.CtlID==ALL_STOP)label=L"\u25cf "+label;
        text_at(dc,area,label,ink,app->font,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    if((item.itemState & ODS_FOCUS) && !(SendMessageW(item.hwndItem,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) focus_ring(dc,area,app->px(2));
}

// Preserve the native combo's keyboard, accessibility and popup behavior.
// Paint its closed face and the native popup with the same card palette.
static LRESULT CALLBACK popup_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR) {
    if (message == WM_SIZE) {
        RECT area; GetWindowRect(window, &area);
        SetWindowRgn(window, CreateRoundRectRgn(0,0,area.right-area.left+1,area.bottom-area.top+1,
                                              app->px(14),app->px(14)), TRUE);
    }
    if (message == WM_NCPAINT) {
        HDC dc = GetWindowDC(window); RECT area; GetWindowRect(window, &area);
        OffsetRect(&area, -area.left, -area.top);
        HPEN pen = CreatePen(PS_SOLID,1,RGB(220,228,239)); HGDIOBJ old_pen=SelectObject(dc,pen);
        HGDIOBJ old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        RoundRect(dc,0,0,area.right,area.bottom,app->px(14),app->px(14));
        SelectObject(dc,old_brush); SelectObject(dc,old_pen); DeleteObject(pen); ReleaseDC(window,dc);
        return 0;
    }
    return DefSubclassProc(window,message,wparam,lparam);
}

static LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR) {
    // The dialog manager changes default-button styles while moving focus.
    // Keep the custom renderer when it requests ordinary/default push styles.
    if (message == BM_SETSTYLE) wparam = (wparam & ~static_cast<WPARAM>(BS_TYPEMASK)) | BS_OWNERDRAW;
    if (message == WM_MOUSEMOVE && !GetPropW(window,L"hover")) {
        SetPropW(window,L"hover",reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track);
        InvalidateRect(window,nullptr,FALSE);
    }
    if (message == WM_MOUSELEAVE) { RemovePropW(window,L"hover"); InvalidateRect(window,nullptr,FALSE); }
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    // Owner-draw changes appearance, but Enter must still activate the focused
    // push button through IsDialogMessage's normal command handling.
    return message == WM_GETDLGCODE ? result | (GetFocus() == window ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON) : result;
}

static LRESULT CALLBACK readonly_text_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR, DWORD_PTR) {
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    // Read-only multiline text must not trap Tab inside the log/detail view.
    return message == WM_GETDLGCODE && wparam == VK_TAB ? result & ~(DLGC_WANTALLKEYS | DLGC_WANTTAB) : result;
}

static LRESULT CALLBACK combo_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint = {}; HDC dc = message == WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wparam);
        RECT area; GetClientRect(window,&area); FillRect(dc,&area,app->surface);
        bool enabled=IsWindowEnabled(window), focus=GetFocus()==window;
        RECT frame=area; InflateRect(&frame,-1,-1);
        rounded(dc,frame, enabled ? RGB(255,255,255) : RGB(247,249,252),
                focus ? RGB(111,158,249) : RGB(220,228,239),app->px(16));
        RECT text=area; text.left+=app->px(12); text.right-=app->px(30);
        SelectObject(dc,app->font); SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,enabled ? RGB(58,74,96) : RGB(177,188,205));
        std::wstring value=window_text(window);
        DrawTextW(dc,value.c_str(),-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        int x=area.right-app->px(17), y=area.bottom/2;
        HPEN pen=CreatePen(PS_SOLID,app->px(2),enabled ? RGB(133,151,176) : RGB(190,201,217));
        HGDIOBJ old=SelectObject(dc,pen);
        MoveToEx(dc,x-app->px(4),y-app->px(2),nullptr); LineTo(dc,x,y+app->px(2)); LineTo(dc,x+app->px(4),y-app->px(2));
        SelectObject(dc,old); DeleteObject(pen);
        if (message==WM_PAINT) EndPaint(window,&paint);
        return 0;
    }
    LRESULT result=DefSubclassProc(window,message,wparam,lparam);
    if (message==CB_SETCURSEL || message==CB_SHOWDROPDOWN || message==WM_SETFOCUS || message==WM_KILLFOCUS ||
        message==WM_ENABLE || message==WM_KEYDOWN || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP)
        InvalidateRect(window,nullptr,FALSE);
    return result;
}

static WNDPROC checkbox_proc;
static LRESULT CALLBACK switch_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint);
        RECT area; GetClientRect(window, &area); FillRect(dc, &area, app->surface);
        bool on = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED, enabled = IsWindowEnabled(window);
        RECT track = {area.right-app->px(34), (area.bottom-app->px(18))/2, area.right, (area.bottom+app->px(18))/2};
        COLORREF color = on && enabled ? RGB(43,108,246) : RGB(219,226,238);
        rounded(dc, track, color, color, app->px(18));
        RECT thumb = track; InflateRect(&thumb, -app->px(3), -app->px(3));
        if (on) thumb.left = thumb.right - app->px(12); else thumb.right = thumb.left + app->px(12);
        rounded(dc, thumb, RGB(255,255,255), RGB(255,255,255), app->px(12));
        area.right = track.left-app->px(6); SelectObject(dc, app->font); SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? RGB(88,107,133) : RGB(172,184,202));
        std::wstring text = window_text(window); DrawTextW(dc, text.c_str(), -1, &area, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (GetFocus() == window) focus_ring(dc, area, app->px(2));
        EndPaint(window, &paint); return 0;
    }
    LRESULT result = CallWindowProcW(checkbox_proc, window, message, wparam, lparam);
    if (message == BM_SETCHECK || message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}

static int CALLBACK browse_callback(HWND window, UINT message, LPARAM, LPARAM initial) {
    if (message == BFFM_INITIALIZED) SendMessageW(window, BFFM_SETSELECTIONW, TRUE, initial);
    return 0;
}

static std::wstring choose_folder(HWND owner, const wchar_t *title, const std::wstring &initial) {
    BROWSEINFOW info = {};
    info.hwndOwner = owner; info.lpszTitle = title; info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    info.lpfn = browse_callback; info.lParam = reinterpret_cast<LPARAM>(initial.c_str());
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
    if (!item) return {};
    wchar_t *path = nullptr;
    HRESULT result = SHGetNameFromIDList(item, SIGDN_FILESYSPATH, &path);
    CoTaskMemFree(item);
    if (FAILED(result)) return {};
    std::vector<wchar_t> display(32768);
    DWORD length = GetLongPathNameW(path, display.data(), static_cast<DWORD>(display.size()));
    std::wstring chosen = length && length < display.size() ? std::wstring(display.data(), length) : std::wstring(path);
    CoTaskMemFree(path);
    return chosen;
}

static void update_fonts() {
    HFONT old[] = {app->font,app->strong_font,app->log_font,app->small_font};
    auto font=[](int size,int weight,const wchar_t *face) {
        return CreateFontW(-app->px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
    };
    app->font=font(12,FW_NORMAL,L"Microsoft YaHei UI");
    app->strong_font=font(13,FW_SEMIBOLD,L"Microsoft YaHei UI");
    app->log_font=font(12,FW_NORMAL,L"Consolas");
    app->small_font=font(10,FW_NORMAL,L"Microsoft YaHei UI");
    for(HWND child=GetWindow(app->window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        int id=GetDlgCtrlID(child);HFONT use=app->font;
        if(id==202 || id==207 || id==209 || id==214 || id==SERVICE_DETAIL)use=app->strong_font;
        if(id==201 || id==205 || id==210 || id==212)use=app->small_font;
        if(id==206)use=app->log_font;
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(use),TRUE);
    }
    for(int id:{LOG_SOURCE,LOG_ENCODING}) {
        SendMessageW(GetDlgItem(app->window,id),CB_SETITEMHEIGHT,0,app->px(32));
        SendMessageW(GetDlgItem(app->window,id),CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),app->px(28));
    }
    for(HFONT previous:old)if(previous)DeleteObject(previous);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    try {
        switch (message) {
        case WM_CREATE: {
            app->window = window;
            auto font = [&](int size, int weight, const wchar_t *face) {
                return CreateFontW(-app->px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
            };
            app->font = font(12, FW_NORMAL, L"Microsoft YaHei UI");
            app->strong_font = font(13, FW_SEMIBOLD, L"Microsoft YaHei UI");
            app->small_font = font(10, FW_NORMAL, L"Microsoft YaHei UI");
            app->log_font = font(12, FW_NORMAL, L"Consolas");
            app->background = CreateSolidBrush(RGB(244,247,251)); app->surface = CreateSolidBrush(RGB(255,255,255));
            auto control = [&](const wchar_t *type, const wchar_t *text, DWORD style, int id) {
                bool checkbox = !std::wcscmp(type, L"BUTTON") && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
                if (!std::wcscmp(type, L"BUTTON") && !checkbox) style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
                HWND child = CreateWindowExW(0, type, text,
                    WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                require(child != nullptr, "Create native control");
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
                if (!std::wcscmp(type, L"BUTTON") && !checkbox) SetWindowSubclass(child, button_proc, 1, 0);
                if (!std::wcscmp(type, L"EDIT") && (style & ES_MULTILINE) && (style & ES_READONLY))
                    SetWindowSubclass(child, readonly_text_proc, 1, 0);
                if (!std::wcscmp(type, L"COMBOBOX")) {
                    SetWindowTheme(child,L"",L"");
                    SetWindowSubclass(child,combo_proc,1,0);
                    SendMessageW(child,CB_SETITEMHEIGHT,0,app->px(32));
                    COMBOBOXINFO info={}; info.cbSize=sizeof(info); GetComboBoxInfo(child,&info);
                    SetWindowTheme(info.hwndList,L"",L"");
                    SetWindowLongPtrW(info.hwndList,GWL_EXSTYLE,GetWindowLongPtrW(info.hwndList,GWL_EXSTYLE)&~WS_EX_CLIENTEDGE);
                    SetWindowSubclass(info.hwndList,popup_proc,1,0);
                    SetWindowPos(info.hwndList,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
                }
                if (checkbox) {
                    WNDPROC previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(switch_proc)));
                    if (!checkbox_proc) checkbox_proc = previous;
                }
                return child;
            };
            control(L"STATIC", L"全局控制", SS_CENTERIMAGE, 214);
            control(L"STATIC", L"正在检查资源…", SS_ENDELLIPSIS, 218);
            control(L"BUTTON", L"检查资源", WS_TABSTOP, CHECK_RESOURCES);
            control(L"BUTTON", L"创建实例", WS_TABSTOP, CREATE_INSTANCE);
            control(L"BUTTON", L"删除实例", WS_TABSTOP, DELETE_INSTANCE);
            control(L"STATIC", L"0 / 0 运行中", SS_CENTERIMAGE, 213);
            app->path = control(L"EDIT", app->initial.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, INSTANCE);
            SendMessageW(app->path, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(app->px(8),app->px(6)));
            control(L"BUTTON", L"选择…", WS_TABSTOP, BROWSE);
            control(L"STATIC", L"MySQL", SS_CENTERIMAGE, 202);
            control(L"STATIC", L"5.7.44", SS_CENTERIMAGE, 201);
            control(L"STATIC", L"", SS_CENTERIMAGE|SS_ENDELLIPSIS, 212);
            control(L"STATIC", L"未启动", 0, 215);
            control(L"STATIC", L"端口", 0, 203);
            app->port = control(L"EDIT", L"3306", ES_NUMBER | ES_CENTER | WS_TABSTOP, PORT);
            app->status = control(L"STATIC", L"选择实例目录以开始", SS_LEFT, 204);
            for (const auto &button : std::vector<std::pair<int, const wchar_t *>>{{INITIALIZE,L"初始化"},{START,L"启动"},{STOP,L"停止"},{BACKUP,L"备份…"},{RESTORE,L"还原…"},{RECOVER,L"恢复操作"},{FOLDER,L"实例目录"},{LOGS,L"日志目录"}})
                control(L"BUTTON", button.second, WS_TABSTOP, button.first);
            control(L"STATIC", L"备份与还原会先按逆序停止服务和数据库", SS_ENDELLIPSIS, 205);
            control(L"BUTTON", L"DP2/Lua", BS_AUTOCHECKBOX | WS_TABSTOP, DP2_ENABLE);
            control(L"BUTTON", L"Frida", BS_AUTOCHECKBOX | WS_TABSTOP, FRIDA_ENABLE);
            control(L"BUTTON", L"保存设置", WS_TABSTOP, PLUGINS_SAVE);
            control(L"STATIC", L"停止 game 后修改", 0, 208);
            app->output = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 206);
            SendMessageW(app->output, WM_SETFONT, reinterpret_cast<WPARAM>(app->log_font), TRUE);
            control(L"STATIC", L"服务", SS_CENTERIMAGE, 207);
            control(L"STATIC", L"请选择左侧服务查看详情", SS_ENDELLIPSIS, SERVICE_DETAIL);
            control(L"EDIT", L"服务元数据", ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, SERVICE_META);
            control(L"EDIT", L"就绪层级：未选择", ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, SERVICE_READINESS);
            control(L"STATIC", L"服务 IP", 0, 216);
            app->service_ip = control(L"EDIT", L"127.0.0.1", ES_AUTOHSCROLL | WS_TABSTOP, SERVICE_IP);
            SendMessageW(app->service_ip, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(app->px(6), app->px(6)));
            control(L"BUTTON", L"应用", WS_TABSTOP, IP_APPLY);
            control(L"STATIC", L"全部停止后应用，下次启动生效", 0, 217);
            control(L"LISTBOX", L"", LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, SERVICE_LIST);
            SetWindowSubclass(GetDlgItem(window, SERVICE_LIST), service_grid_proc, 1, 0);
            for (const auto &button : std::vector<std::pair<int, const wchar_t *>>{{ELF_START,L"启动所选"},{ELF_STOP,L"停止所选"},{ALL_START,L"整体启动"},{ALL_STOP,L"整体停止"},{SERVICE_LOGS,L"查看所选日志"}})
                control(L"BUTTON", button.second, WS_TABSTOP | ((button.first == ALL_START || button.first == ALL_STOP) ? BS_OWNERDRAW : 0), button.first);
            control(L"STATIC", L"原始日志", 0, 209);
            control(L"STATIC", L"", SS_PATHELLIPSIS | SS_NOPREFIX, 210);
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, LOG_SOURCE);
            HWND encoding = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, LOG_ENCODING);
            for (auto label : {L"自动编码", L"UTF-8", L"简体中文 / 936", L"繁体中文 / 950", L"韩文 / 949"})
                SendMessageW(encoding, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(encoding, CB_SETCURSEL, 0, 0);
            SendMessageW(GetDlgItem(window, LOG_SOURCE), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), app->px(28));
            SendMessageW(encoding, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), app->px(28));
            control(L"BUTTON", L"跟随滚动", BS_AUTOCHECKBOX | WS_TABSTOP, LOG_FOLLOW);
            SendMessageW(GetDlgItem(window, LOG_FOLLOW), BM_SETCHECK, BST_CHECKED, 0);
            control(L"BUTTON", L"刷新", WS_TABSTOP, LOG_REFRESH);
            control(L"BUTTON", L"日志目录", WS_TABSTOP, LOG_FOLDER);
            control(L"EDIT", L"维护摘要：尚未执行", ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, MAINTENANCE_DETAIL);
            for (int id : {202,207,209,214,static_cast<int>(SERVICE_DETAIL)})
                SendMessageW(GetDlgItem(window, id), WM_SETFONT, reinterpret_cast<WPARAM>(app->strong_font), TRUE);
            SendMessageW(app->output, EM_LIMITTEXT, 4 * 1024 * 1024, 0);
            control(L"BUTTON", L"⋯", WS_TABSTOP, INSTANCE_MORE);
            control(L"BUTTON", L"详情", WS_TABSTOP, SERVICE_INFO);
            control(L"BUTTON", L"详情", WS_TABSTOP, DATABASE_INFO);
            control(L"EDIT", L"", ES_AUTOHSCROLL|WS_TABSTOP, LOG_FILTER);
            SendMessageW(GetDlgItem(window,LOG_FILTER), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"过滤关键字…"));
            for (const auto &tab : std::vector<std::pair<int,const wchar_t *>>{{LOG_CONSOLE_TAB,L"控制台操作"},{LOG_SERVICE_TAB,L"服务日志"},{LOG_ERROR_TAB,L"宿主诊断"},{LOG_MYSQL_TAB,L"MySQL"},{LOG_FILES,L"日志文件 ⌄"}})
                control(L"BUTTON",tab.second,WS_TABSTOP,tab.first);
            for (int id : std::initializer_list<int>{214,218,215,216,217,208,209,SERVICE_DETAIL,SERVICE_META,SERVICE_READINESS,MAINTENANCE_DETAIL,INITIALIZE,DELETE_INSTANCE,LOGS,LOG_SOURCE,LOG_FOLDER})
                ShowWindow(GetDlgItem(window,id),SW_HIDE);
            SendMessageW(GetDlgItem(window,201),WM_SETFONT,reinterpret_cast<WPARAM>(app->small_font),FALSE);
            SendMessageW(GetDlgItem(window,212),WM_SETFONT,reinterpret_cast<WPARAM>(app->small_font),FALSE);
            SendMessageW(GetDlgItem(window,205),WM_SETFONT,reinterpret_cast<WPARAM>(app->small_font),FALSE);
            SendMessageW(GetDlgItem(window,210),WM_SETFONT,reinterpret_cast<WPARAM>(app->small_font),FALSE);
            HWND tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,
                                        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,nullptr,nullptr);
            for(auto tip:{std::pair<int,const wchar_t *>{BROWSE,L"选择或切换本地实例目录"},{CREATE_INSTANCE,L"在空目录创建新实例"},{FOLDER,L"打开实例根目录"},{CHECK_RESOURCES,L"核对交付资源与校验清单"},{INSTANCE_MORE,L"初始化、删除实例及维护日志"},{SERVICE_LIST,L"单击选择 · 双击日志 · 右键操作与详情"},{LOG_FILES,L"选择应用日志、标准输出或宿主诊断"}}) {
                TOOLINFOW info={};info.cbSize=sizeof(info);info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
                info.hwnd=window;info.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window,tip.first));info.lpszText=const_cast<wchar_t *>(tip.second);
                SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
            }
            app->check_resources(); app->load(); SetTimer(window, 1, 500, nullptr);
            return 0;
        }
        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) return 0;
            int width=MulDiv(LOWORD(lparam),96,app->dpi),height=MulDiv(HIWORD(lparam),96,app->dpi);
            auto place=[&](int id,int x,int y,int w,int h) {MoveWindow(GetDlgItem(window,id),app->px(x),app->px(y),app->px(std::max(0,w)),app->px(std::max(0,h)),TRUE);};
            bool compact=width<1330;
            int selector=compact?200:236;
            place(BROWSE,32,27,selector,46);place(CREATE_INSTANCE,44+selector,35,30,30);
            SetWindowTextW(GetDlgItem(window,CREATE_INSTANCE),L"+");
            int right=width-32;
            auto button=[&](int id,int w) {right-=w;place(id,right,35,w,30);right-=10;};
            button(ALL_START,96);button(ALL_STOP,96);button(INSTANCE_MORE,30);
            button(FOLDER,compact?34:108);button(CHECK_RESOURCES,compact?34:92);
            SetWindowTextW(GetDlgItem(window,FOLDER),compact?L"\u25a1":L"\u25a1  \u5b9e\u4f8b\u6839\u76ee\u5f55");
            SetWindowTextW(GetDlgItem(window,CHECK_RESOURCES),compact?L"\u2713":L"\u2713  \u68c0\u67e5\u8d44\u6e90");
            right-=compact?134:174;
            place(SERVICE_IP,right+(compact?6:44),41,84,20);place(IP_APPLY,right+(compact?94:132),35,36,30);
            int path_left=68+selector;
            place(INSTANCE,path_left+8,41,right-path_left-22,20);
            // Seven columns and three rows share their hit rectangles with the
            // native selection model; metadata moves to the real detail action.
            place(207,36,108,34,22);place(213,76,108,140,22);
            place(DP2_ENABLE,width-574,105,90,26);place(FRIDA_ENABLE,width-474,105,74,26);
            place(PLUGINS_SAVE,width-390,103,76,30);place(SERVICE_INFO,width-310,103,46,30);
            place(ELF_START,width-254,103,68,30);place(ELF_STOP,width-178,103,68,30);place(SERVICE_LOGS,width-102,103,68,30);
            SetWindowTextW(GetDlgItem(window,SERVICE_LOGS),L"\u67e5\u770b\u65e5\u5fd7");
            place(SERVICE_LIST,36,150,width-72,176);
            place(202,72,375,54,28);place(201,128,376,48,26);place(203,198,376,28,26);place(PORT,232,380,48,20);
            place(212,296,376,std::max(0,width-826),26);
            right=width-34;
            auto db_button=[&](int id,int w) {right-=w;place(id,right,375,w,30);right-=8;};
            db_button(DATABASE_INFO,42);db_button(RECOVER,76);db_button(RESTORE,76);db_button(BACKUP,76);db_button(START,54);db_button(STOP,54);
            place(205,38,423,width-76,20);
            int log_top=462,log_bottom=height-46;
            place(LOG_CONSOLE_TAB,28,log_top+6,96,38);place(LOG_SERVICE_TAB,128,log_top+6,92,38);
            place(LOG_ERROR_TAB,224,log_top+6,86,38);place(LOG_MYSQL_TAB,314,log_top+6,74,38);place(LOG_FILES,400,log_top+10,94,30);
            place(LOG_FILTER,width-500,log_top+16,170,20);place(LOG_ENCODING,width-312,log_top+9,100,30);
            place(LOG_REFRESH,width-204,log_top+10,50,30);place(LOG_FOLLOW,width-144,log_top+12,110,26);
            place(206,36,log_top+54,width-72,std::max(0,log_bottom-log_top-86));
            place(210,40,log_bottom-25,width-80,18);place(204,32,height-28,width-64,20);
            for(int id:{INITIALIZE,DELETE_INSTANCE,LOGS,LOG_SOURCE})ShowWindow(GetDlgItem(window,id),SW_HIDE);
            InvalidateRect(window,nullptr,TRUE);return 0;
        }
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wparam);
            update_fonts();
            RECT suggested = *reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, suggested.left, suggested.top,
                         suggested.right - suggested.left, suggested.bottom - suggested.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {app->px(1180), app->px(760)}; return 0;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT *>(lparam)->itemHeight=app->px(32); return TRUE;
        case WM_DRAWITEM:
            draw_item(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam)); return TRUE;
        case WM_ERASEBKGND: {
            RECT area; GetClientRect(window, &area); FillRect(reinterpret_cast<HDC>(wparam), &area, app->background); return TRUE;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint;HDC target=BeginPaint(window,&paint);RECT area;GetClientRect(window,&area);
            HDC dc=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,area.right),std::max(1L,area.bottom));
            HGDIOBJ old=SelectObject(dc,bitmap);FillRect(dc,&area,app->background);
            auto card=[&](int top,int bottom) {RECT panel={app->px(20),app->px(top),area.right-app->px(20),app->px(bottom)};rounded(dc,panel,RGB(255,255,255),RGB(228,234,245),app->px(14));};
            int width=MulDiv(area.right,96,app->dpi),height=MulDiv(area.bottom,96,app->dpi);
            card(16,84);card(96,342);card(354,450);card(462,height-46);
            bool compact=width<1330;int selector=compact?200:236;
            int right=width-32-(96+10+96+10+30+10+(compact?34:108)+10+(compact?34:92)+10)-(compact?134:174);
            RECT path={app->px(68+selector),app->px(33),app->px(right-8),app->px(66)};
            rounded(dc,path,RGB(248,250,254),RGB(228,235,248),app->px(8));
            RECT ip={app->px(right),app->px(35),app->px(right+(compact?134:174)),app->px(65)};
            rounded(dc,ip,RGB(255,255,255),RGB(220,230,247),app->px(8));
            if(!compact) {RECT label={ip.left+app->px(6),ip.top,ip.left+app->px(42),ip.bottom};text_at(dc,label,L"\u670d\u52a1 IP",RGB(139,157,184),app->small_font);}
            RECT port={app->px(226),app->px(375),app->px(286),app->px(405)};
            rounded(dc,port,RGB(255,255,255),RGB(220,230,247),app->px(8));
            RECT search={app->px(width-508),app->px(472),app->px(width-322),app->px(502)};
            rounded(dc,search,RGB(251,252,255),RGB(220,230,247),app->px(8));
            RECT icon={app->px(36),app->px(375),app->px(62),app->px(403)};
            rounded(dc,icon,RGB(242,246,255),RGB(242,246,255),app->px(8));
            HPEN pen=CreatePen(PS_SOLID,1,RGB(43,96,239));HGDIOBJ old_pen=SelectObject(dc,pen),old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            for(int y:{381,387,393}) {Ellipse(dc,app->px(42),app->px(y),app->px(56),app->px(y+6));}
            MoveToEx(dc,app->px(42),app->px(384),nullptr);LineTo(dc,app->px(42),app->px(396));
            MoveToEx(dc,app->px(56),app->px(384),nullptr);LineTo(dc,app->px(56),app->px(396));
            SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(pen);
            dot_at(dc,app->px(184),app->px(386),app->px(6),app->session.process?RGB(24,173,94):RGB(194,204,219));
            RECT hint={app->px(22),app->px(418),area.right-app->px(22),app->px(448)};
            HBRUSH pale=CreateSolidBrush(RGB(251,252,255));FillRect(dc,&hint,pale);DeleteObject(pale);
            RECT footer={0,area.bottom-app->px(32),area.right,area.bottom};FillRect(dc,&footer,app->surface);
            BitBlt(target,0,0,area.right,area.bottom,dc,0,0,SRCCOPY);
            SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);EndPaint(window,&paint);return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam)); HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, id == 216 || id == 217 || id == 210 || id == 205 || id == 208 || id == 212 || id == 201 || id == 215 || id == 204 ? RGB(139,155,178) : id == 213 ? RGB(39,151,111) : RGB(35,48,66));
            SetBkColor(dc, RGB(255,255,255));
            return reinterpret_cast<LRESULT>(app->surface);
        }
        case PROGRESS_MESSAGE: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lparam));
            SetWindowTextW(app->status,text->c_str()); app->append(*text); return 0;
        }
        case STATUS_MESSAGE:
            SetWindowTextW(app->status, reinterpret_cast<const wchar_t *>(lparam)); return 0;
        case DONE_MESSAGE: {
            std::unique_ptr<Result> result(reinterpret_cast<Result *>(lparam));
            app->worker.join(); app->busy = false;
            if (result->stopped && app->session.process) { CloseHandle(app->session.process); app->session = {}; }
            if (result->started.process) app->session = result->started;
            if (result->deleted) {
                app->current.clear(); app->services.clear();
                SetWindowTextW(app->path, L""); app->show_plugins();
            }
            if (result->created) { app->services = load_services(app->current, app->runtime); app->show_plugins(); }
            if (!result->maintenance.empty()) app->maintenance_summary = result->maintenance;
            app->show_services(); SetWindowTextW(app->status, result->status.c_str()); app->append(result->details); app->controls(); app->scan_logs();
            if (!result->success) {
                for (size_t i=0;i<app->log_sources.size();++i) if (app->log_sources[i].path.empty()) {
                    SendMessageW(GetDlgItem(window,LOG_SOURCE),CB_SETCURSEL,i,0);app->refresh_log(true);break;
                }
            }
            if (app->closing && result->success && !app->session.process && !app->services_running()) PostMessageW(window, WM_CLOSE, 0, 0);
            else app->closing = false;
            return 0;
        }
        case WM_TIMER:
            if (!app->busy) {
                bool changed = false;
                for (auto &service : app->services) {
                    if (refresh_service(service)) {
                        app->append(service.name + L"：" + service.state + L"\r\n诊断日志：" + service.err); changed = true;
                    } else if (refresh_service_readiness(service)) {
                        app->append(service.name + L"：" + service.readiness); changed = true;
                    }
                }
                if (changed) { app->show_services(); app->controls(); }
            }
            if (!app->busy && app->session.process && WaitForSingleObject(app->session.process, 0) == WAIT_OBJECT_0) {
                std::wstring text = L"数据库进程已退出，退出码 " + std::to_wstring(exit_code(app->session.process));
                app->append(text + L"\r\n服务器日志：" + app->session.log); SetWindowTextW(app->status, text.c_str());
                CloseHandle(app->session.process); app->session = {}; app->controls();
            }
            if (GetTickCount64() - app->log_scan >= 2000) { app->scan_logs(); app->log_scan = GetTickCount64(); }
            app->refresh_log();
            return 0;
        case WM_CONTEXTMENU: {
            if (reinterpret_cast<HWND>(wparam) != GetDlgItem(window,SERVICE_LIST) || app->busy) break;
            POINT point={static_cast<short>(LOWORD(lparam)),static_cast<short>(HIWORD(lparam))};
            if (point.x==-1 && point.y==-1) {
                RECT tile=service_tile(GetDlgItem(window,SERVICE_LIST),std::max(0,app->selected_service_index()));
                point={tile.left,tile.bottom};ClientToScreen(GetDlgItem(window,SERVICE_LIST),&point);
            } else {
                POINT local=point;ScreenToClient(GetDlgItem(window,SERVICE_LIST),&local);
                LRESULT hit=SendMessageW(GetDlgItem(window,SERVICE_LIST),LB_ITEMFROMPOINT,0,MAKELPARAM(local.x,local.y));
                if (HIWORD(hit)) return 0;
                SendMessageW(GetDlgItem(window,SERVICE_LIST),LB_SETCURSEL,LOWORD(hit),0);
                SendMessageW(window,WM_COMMAND,MAKEWPARAM(SERVICE_LIST,LBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(window,SERVICE_LIST)));
            }
            HMENU menu=CreatePopupMenu();
            for (auto entry : {std::pair<int,const wchar_t *>{ELF_START,L"启动所选"},{ELF_STOP,L"停止所选"},{SERVICE_LOGS,L"查看日志"},{SERVICE_INFO,L"服务详情"}})
                AppendMenuW(menu,MF_STRING|(IsWindowEnabled(GetDlgItem(window,entry.first))?MF_ENABLED:MF_GRAYED),entry.first,entry.second);
            int chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);
            if(chosen)SendMessageW(window,WM_COMMAND,chosen,0);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id==LOG_FILTER && HIWORD(wparam)==EN_CHANGE) {app->refresh_log(true);return 0;}
            if (id == INSTANCE && HIWORD(wparam) == EN_CHANGE) { if (app->status && app->output) app->load(); return 0; }
            if (id == SERVICE_LIST && HIWORD(wparam) == LBN_SELCHANGE && !app->busy) {
                int selected = app->selected_service_index();
                app->preferred_service = selected >= 0 ? app->services[static_cast<size_t>(selected)].name : L"";
                app->show_service_detail(); app->controls(); app->scan_logs(true); return 0;
            }
            if (id == SERVICE_LIST && HIWORD(wparam) == LBN_DBLCLK) { app->scan_logs(true); return 0; }
            if ((id == LOG_SOURCE || id == LOG_ENCODING) && HIWORD(wparam) == CBN_SELCHANGE) { app->refresh_log(true); return 0; }
            if (id == LOG_REFRESH) { app->scan_logs(); app->refresh_log(true); return 0; }
            if (id == LOG_FOLLOW) { app->refresh_log(true); return 0; }
            if (id == LOG_FOLDER) {
                LRESULT index = SendMessageW(GetDlgItem(window, LOG_SOURCE), CB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<size_t>(index) < app->log_sources.size() && !app->log_sources[index].path.empty()) {
                    std::wstring path = app->log_sources[index].path; path.resize(path.find_last_of(L"\\/"));
                    ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            }
            if (HIWORD(wparam) != BN_CLICKED) return 0;
            if (id == CHECK_RESOURCES && !app->busy) {
                app->check_resources(); app->controls();
                app->append(window_text(GetDlgItem(window,218)));
                SetWindowTextW(app->status,window_text(GetDlgItem(window,218)).c_str());return 0;
            }
            if (id==INSTANCE_MORE) {
                HMENU menu=CreatePopupMenu();
                for(auto entry:{std::pair<int,const wchar_t *>{INITIALIZE,L"初始化当前目录(&I)"},{DELETE_INSTANCE,L"删除当前实例(&D)…"},{LOGS,L"打开维护日志目录(&L)"}})
                    AppendMenuW(menu,MF_STRING|(IsWindowEnabled(GetDlgItem(window,entry.first))?MF_ENABLED:MF_GRAYED),entry.first,entry.second);
                RECT button;GetWindowRect(GetDlgItem(window,INSTANCE_MORE),&button);
                int chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTALIGN,button.right,button.bottom,0,window,nullptr);DestroyMenu(menu);
                if(chosen)SendMessageW(window,WM_COMMAND,chosen,0);return 0;
            }
            if(id==SERVICE_INFO) {
                std::wstring text=window_text(GetDlgItem(window,SERVICE_DETAIL))+L"\n\n"+window_text(GetDlgItem(window,SERVICE_META))+L"\n\n"+window_text(GetDlgItem(window,SERVICE_READINESS));
                MessageBoxW(window,text.c_str(),L"服务详情",MB_OK|MB_ICONINFORMATION);return 0;
            }
            if(id==DATABASE_INFO) {
                app->show_maintenance_detail();MessageBoxW(window,window_text(GetDlgItem(window,MAINTENANCE_DETAIL)).c_str(),L"数据库与维护详情",MB_OK|MB_ICONINFORMATION);return 0;
            }
            if(id==LOG_FILES) {
                HMENU menu=CreatePopupMenu();
                for(size_t i=0;i<app->log_sources.size();++i)AppendMenuW(menu,MF_STRING,1000+i,app->log_sources[i].label.c_str());
                AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                AppendMenuW(menu,MF_STRING,LOG_FOLDER,L"打开当前日志目录");
                RECT button;GetWindowRect(GetDlgItem(window,LOG_FILES),&button);
                int chosen=TrackPopupMenu(menu,TPM_RETURNCMD,button.left,button.bottom,0,window,nullptr);DestroyMenu(menu);
                if(chosen>=1000 && static_cast<size_t>(chosen-1000)<app->log_sources.size()) {
                    SendMessageW(GetDlgItem(window,LOG_SOURCE),CB_SETCURSEL,chosen-1000,0);app->refresh_log(true);
                } else if(chosen==LOG_FOLDER)SendMessageW(window,WM_COMMAND,LOG_FOLDER,0);
                return 0;
            }
            if(id>=LOG_CONSOLE_TAB && id<=LOG_MYSQL_TAB) {
                auto matches=[&](const LogSource &source) {
                    if(id==LOG_CONSOLE_TAB)return source.path.empty();
                    if(id==LOG_MYSQL_TAB)return source.label==L"MySQL 原生日志";
                    if(id==LOG_ERROR_TAB)return source.label.find(L"标准错误")==0;
                    return !source.path.empty()&&source.label!=L"MySQL 原生日志"&&source.label.find(L"标准错误")!=0;
                };
                auto found=std::find_if(app->log_sources.begin(),app->log_sources.end(),matches);
                if(found!=app->log_sources.end()) {
                    SendMessageW(GetDlgItem(window,LOG_SOURCE),CB_SETCURSEL,found-app->log_sources.begin(),0);app->refresh_log(true);
                } else SetWindowTextW(app->status,L"当前实例尚无这类日志");
                return 0;
            }
            if (id == CREATE_INSTANCE && !app->busy) {
                auto chosen = choose_folder(window, L"选择用于新实例的空文件夹（可在此新建文件夹）", app->current);
                if (!chosen.empty()) {
                    SetWindowTextW(app->path, chosen.c_str());
                    if (!known_instance(full_path(chosen))) app->action(CREATE_INSTANCE);
                    else SetWindowTextW(app->status, L"该目录已有实例，请选择新的空文件夹");
                }
                return 0;
            }
            if (id == DELETE_INSTANCE && !app->busy) {
                if (app->services_running() || app->session.process || !known_instance(app->current)) return 0;
                std::wstring question = L"永久删除整个实例文件夹及全部内容？\n\n" + app->current + L"\n\n此操作无法撤销。";
                if (MessageBoxW(window, question.c_str(), L"删除实例", MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONWARNING) == IDOK)
                    app->action(DELETE_INSTANCE);
                return 0;
            }
            if (id == SERVICE_LOGS) { app->scan_logs(true); return 0; }
            if (id == FOLDER || id == LOGS) {
                std::wstring path = full_path(window_text(app->path)) + (id == LOGS ? L"\\.mysql" : L"");
                ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
            }
            if (app->busy) return 0;
            if (id == DP2_ENABLE) {
                if (SendMessageW(GetDlgItem(window, DP2_ENABLE), BM_GETCHECK, 0, 0) != BST_CHECKED)
                    SendMessageW(GetDlgItem(window, FRIDA_ENABLE), BM_SETCHECK, BST_UNCHECKED, 0);
                app->controls();
            } else if (id == FRIDA_ENABLE) app->controls();
            else if (id == PLUGINS_SAVE) app->save_plugins();
            else if (id == IP_APPLY) {
                std::wstring value = window_text(app->service_ip);
                if (!valid_ipv4(value)) {
                    SetWindowTextW(app->status, L"服务 IP 必须是有效的 IPv4 地址");
                    return 0;
                }
                app->action(IP_APPLY, value);
            }
            else if (id == BROWSE) {
                HMENU menu=CreatePopupMenu();
                for(size_t i=0;i<app->recent_instances.size();++i) {
                    const auto &path=app->recent_instances[i];
                    UINT flags=MF_STRING|(path==app->current?MF_CHECKED:0);
                    std::wstring label=path;
                    for(size_t at=0;(at=label.find(L'&',at))!=std::wstring::npos;at+=2)label.insert(at,1,L'&');
                    label=(i<9?L"&"+std::to_wstring(i+1)+L"  ":L"")+label;
                    AppendMenuW(menu,flags,2000+i,label.c_str());
                }
                if(!app->recent_instances.empty())AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                AppendMenuW(menu,MF_STRING,1,L"选择其他实例目录(&O)…");
                AppendMenuW(menu,MF_STRING|(IsWindowEnabled(GetDlgItem(window,CREATE_INSTANCE))?MF_ENABLED:MF_GRAYED),2,L"新建实例(&N)…");
                RECT button;GetWindowRect(GetDlgItem(window,BROWSE),&button);
                int selected=TrackPopupMenu(menu,TPM_RETURNCMD,button.left,button.bottom,0,window,nullptr);DestroyMenu(menu);
                if(selected==2) {SendMessageW(window,WM_COMMAND,CREATE_INSTANCE,0);return 0;}
                std::wstring chosen;
                if(selected==1)chosen=choose_folder(window,L"选择本地实例目录",app->current);
                else if(selected>=2000 && static_cast<size_t>(selected-2000)<app->recent_instances.size())chosen=app->recent_instances[selected-2000];
                if (!chosen.empty()) { SetWindowTextW(app->path, chosen.c_str()); app->load(); }
            } else if (id == FOLDER || id == LOGS) {
                std::wstring path = full_path(window_text(app->path)) + (id == LOGS ? L"\\.mysql" : L"");
                ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == BACKUP || id == RESTORE) {
                auto target = choose_folder(window, id == BACKUP ? L"选择备份保存位置（将新建备份目录）" : L"选择要还原的备份目录", app->current);
                if (target.empty()) return 0;
                if (id == BACKUP) {
                    SYSTEMTIME t; GetLocalTime(&t); wchar_t name[64];
                    std::swprintf(name, 64, L"\\backup-%04u%02u%02u-%02u%02u%02u", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
                    target += name;
                } else {
                    std::wstring preview = L"还原当前实例数据库？\n\n实例：" + app->current + L"\n数据库：" + app->database_identity() +
                                           L"\n备份：" + target + L"\n逆序停服：" + app->stop_plan(false) +
                                           L"\n\n先保存预备份，在影子副本验证后切换；保留原数据冷快照。";
                    if (MessageBoxW(window, preview.c_str(), L"确认还原", MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDOK) return 0;
                }
                app->action(id, target);
            } else if (id == SERVICE_LOGS) {
                app->scan_logs(true);
            } else if (id >= ELF_START && id <= ALL_STOP)
                app->action(id, L"", app->selected_service_index());
            else if (id >= INITIALIZE && id <= RECOVER) app->action(id);
            return 0;
        }
        case WM_CLOSE:
            if (app->busy) { SetWindowTextW(app->status, L"正在执行操作，请等待完成后关闭"); return 0; }
            if (app->services_running()) { app->closing = true; app->action(ALL_STOP); return 0; }
            if (app->session.process) { app->closing = true; app->action(STOP); return 0; }
            DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, 1); app->close_instance_lease();
            for (HFONT font : {app->font, app->strong_font, app->log_font, app->small_font}) DeleteObject(font);
            DeleteObject(app->background); DeleteObject(app->surface); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception &error) {
        MessageBoxW(window, decode(error.what()).c_str(), L"DNF Native", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetProcessDPIAware();
        App state; app = &state;
        HDC screen = GetDC(nullptr); state.dpi = GetDeviceCaps(screen, LOGPIXELSY); ReleaseDC(nullptr, screen);
        WSADATA sockets; require(WSAStartup(MAKEWORD(2,2), &sockets) == 0, "Initialize service networking");
        state.service_job = CreateJobObjectW(nullptr, nullptr); require(state.service_job != nullptr, "Create ELF host job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(state.service_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Configure ELF host ownership");
        wchar_t module[32768]; require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Find native application directory");
        std::wstring gui_folder(module); gui_folder.resize(gui_folder.find_last_of(L'\\'));
        state.runtime = gui_folder + L"\\resources";
        state.exe = state.runtime + L"\\mysql-control.exe";
        state.resources = state.runtime;
        state.initial = L"";
        int count; wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
        require(args != nullptr, "Read application arguments");
        if (count == 3 && std::wstring(args[1]) == L"--instance") {
            state.initial = args[2];
            full_path(state.initial);
        }
        else if (count != 1) { LocalFree(args); throw std::runtime_error("Usage: DNF-Native.exe [--instance DIRECTORY]"); }
        LocalFree(args);
        WNDCLASSW type = {}; type.hInstance = instance; type.lpfnWndProc = window_proc; type.lpszClassName = L"DNFNativeController";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        require(RegisterClassW(&type) != 0, "Register native window");
        HWND window = CreateWindowExW(0, type.lpszClassName, L"DNF Native 控制台", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                       CW_USEDEFAULT,CW_USEDEFAULT,state.px(1440),state.px(940),nullptr,nullptr,instance,nullptr);
        require(window != nullptr, "Create native window");
        ShowWindow(window, show); UpdateWindow(window);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        CoUninitialize();
        CloseHandle(state.service_job); WSACleanup();
        return 0;
    } catch (const std::exception &error) {
        MessageBoxW(nullptr, decode(error.what()).c_str(), L"DNF Native", MB_OK | MB_ICONERROR);
        return 1;
    }
}
