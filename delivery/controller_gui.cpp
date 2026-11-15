// Independent Win32 console for original ELF hosts and the official MySQL tools.
#include "service_processes.h"
#include <ws2tcpip.h>
#include <uxtheme.h>
#include <commctrl.h>
#include "log_view.h"
#include <algorithm>
#include <cwchar>
#include <memory>
#include <thread>

enum { INSTANCE = 101, BROWSE, PORT, INITIALIZE = 110, START, STOP, BACKUP, RESTORE, RECOVER, FOLDER, LOGS,
       SERVICE_LIST = 220, ELF_START, ELF_STOP, ALL_START, ALL_STOP, SERVICE_LOGS,
       DP2_ENABLE = 230, FRIDA_ENABLE, PLUGINS_SAVE,
       LOG_SOURCE = 240, LOG_ENCODING, LOG_FOLLOW, LOG_REFRESH, LOG_FOLDER,
       SERVICE_IP = 250, IP_APPLY };
enum { STATUS_MESSAGE = WM_APP + 1, DONE_MESSAGE, PROGRESS_MESSAGE };

static bool present(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring full_path(const std::wstring &value) {
    if (value.empty()) throw std::runtime_error("Choose an instance directory first");
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    require(size && size < buffer.size(), "Resolve instance path");
    std::wstring result(buffer.data(), size);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}

#include "service_ip.h"

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

struct Result { bool success = false, stopped = false, created = false; Session started; std::wstring status, details; };

struct App {
    HWND window = nullptr, path = nullptr, port = nullptr, service_ip = nullptr, status = nullptr, output = nullptr;
    HFONT font = nullptr, title_font = nullptr, strong_font = nullptr, log_font = nullptr;
    HBRUSH background = nullptr, surface = nullptr;
    std::wstring exe, initial, current, resources, runtime;
    Session session;
    std::vector<Service> services;
    HANDLE service_job = nullptr;
    bool busy = false, closing = false;
    int operation = 0;
    std::thread worker;
    std::vector<LogSource> log_sources;
    std::wstring operations, shown_log;
    ULONGLONG log_size = 0, log_modified = 0, log_scan = 0;
    bool log_loaded = false;
    int dpi = 96;
    int px(int value) const { return MulDiv(value, dpi, 96); }
    int grid_columns(int width) const { return std::max(1,(width-72)/(width < 1280 ? 140 : 126)); }
    int grid_rows = 2;
    int database_top() const { return 232 + grid_rows * 54; }

    void display_log(const std::wstring &text) {
        bool follow = SendMessageW(GetDlgItem(window, LOG_FOLLOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
        DWORD first = 0, last = 0;
        SendMessageW(output, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        int line = static_cast<int>(SendMessageW(output, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(output, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(output, text.c_str());
        if (follow) SendMessageW(output, EM_SETSEL, text.size(), text.size());
        else {
            SendMessageW(output, EM_SETSEL, std::min<size_t>(first, text.size()), std::min<size_t>(last, text.size()));
        }
        SendMessageW(output, WM_SETREDRAW, TRUE, 0);
        if (follow) SendMessageW(output, EM_SCROLLCARET, 0, 0);
        else SendMessageW(output, EM_LINESCROLL, 0, line);
        InvalidateRect(output, nullptr, TRUE);
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
        if (log_sources.size() == 1 && log_sources[0].path.empty()) reset = true;
        std::wstring previous;
        LRESULT old = SendMessageW(GetDlgItem(window, LOG_SOURCE), CB_GETCURSEL, 0, 0);
        if (!reset && old >= 0 && static_cast<size_t>(old) < log_sources.size()) previous = log_sources[old].path;
        std::vector<LogSource> sources;
        LRESULT selected = SendMessageW(GetDlgItem(window, SERVICE_LIST), LB_GETCURSEL, 0, 0);
        if (selected >= 0 && static_cast<size_t>(selected) < services.size()) {
            const Service &service = services[selected];
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
    void show_services() {
        HWND list = GetDlgItem(window, SERVICE_LIST);
        LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0); SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto &service : services) {
            std::wstring row = service.name + (service.automatic ? L"" : L"（可选）") + L"  |  " + service.state;
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
        if (!services.empty()) SendMessageW(list, LB_SETCURSEL, selected == LB_ERR ? 0 : selected, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
        unsigned live = 0;
        for (const auto &service : services) if (service.process && WaitForSingleObject(service.process, 0) == WAIT_TIMEOUT) ++live;
        std::wstring count = std::to_wstring(live) + L" / " + std::to_wstring(services.size()) + L" 运行中";
        SetWindowTextW(GetDlgItem(window, 213), count.c_str());
        RECT area; GetClientRect(window, &area);
        int columns = grid_columns(MulDiv(area.right,96,dpi));
        if (std::max(2,(static_cast<int>(services.size())+columns-1)/columns) != grid_rows)
            SendMessageW(window,WM_SIZE,SIZE_RESTORED,MAKELPARAM(area.right,area.bottom));
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
        SetWindowTextW(GetDlgItem(window, 215), busy ? L"处理中…" : session.process ? L"运行中" : L"未启动");
        // The worker may be changing process ownership. Inspect it after join.
        if (busy) {
            for (int id : {INSTANCE,BROWSE,PORT,INITIALIZE,START,STOP,BACKUP,RESTORE,RECOVER,
                           SERVICE_IP,IP_APPLY,ELF_START,ELF_STOP,ALL_START,ALL_STOP,DP2_ENABLE,FRIDA_ENABLE,PLUGINS_SAVE})
                EnableWindow(GetDlgItem(window, id), FALSE);
            for (int id : {FOLDER,LOGS,SERVICE_LIST,SERVICE_LOGS}) EnableWindow(GetDlgItem(window,id), !current.empty());
            SetWindowTextW(GetDlgItem(window,ALL_START),operation==ALL_START ? L"正在启动…" : L"整体启动");
            SetWindowTextW(GetDlgItem(window,ALL_STOP),operation==ALL_STOP ? L"正在停止…" : L"整体停止");
            return;
        }
        SetWindowTextW(GetDlgItem(window,ALL_START),L"整体启动");
        SetWindowTextW(GetDlgItem(window,ALL_STOP),L"整体停止");
        bool active = services_running();
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
        LRESULT index = SendMessageW(GetDlgItem(window, SERVICE_LIST), LB_GETCURSEL, 0, 0);
        bool selected = index >= 0 && static_cast<size_t>(index) < services.size();
        EnableWindow(GetDlgItem(window, SERVICE_LIST), !services.empty());
        EnableWindow(GetDlgItem(window, ELF_START), selected && session.process && !running(services[index]) && !pending);
        EnableWindow(GetDlgItem(window, ELF_STOP), selected && running(services[index]));
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
            current.clear(); services.clear(); show_services(); show_plugins(); scan_logs(true); SetWindowTextW(status, L"请选择实例目录"); controls(); return;
        }
        current = full_path(value);
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
        operation = id; busy = true; controls();
        SetWindowTextW(status, L"正在执行操作…");
        worker = std::thread([this, id, instance, tool, target, port_text, owned, selected, assets] {
            auto result = std::make_unique<Result>();
            try {
                if (id == INITIALIZE && present(assets + L"\\resources.ini")) {
                    if (port_text != L"3306") throw std::runtime_error("Original bridge requires database port 3306 for a game instance");
                    create_game_instance(window, tool, assets, instance, result->started);
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
                            for (auto item = services.rbegin(); item != services.rend(); ++item) stop_service(*item);
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
                            for (auto item = services.rbegin(); item != services.rend(); ++item) stop_service(*item);
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
                }
                result->success = true;
            } catch (const std::exception &error) {
                result->status = L"操作失败：" + decode(error.what());
                result->details += L"\r\n" + result->status;
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

static void draw_item(const DRAWITEMSTRUCT &item) {
    HDC dc = item.hDC;
    RECT area = item.rcItem;
    bool selected = (item.itemState & ODS_SELECTED) != 0;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF fill = RGB(255,255,255), ink = RGB(58,74,96), border = RGB(220,228,239);
    FillRect(dc, &area, app->surface);
    if (item.CtlID == ALL_START) { fill = disabled ? RGB(235,239,246) : selected ? RGB(29,83,203) : RGB(43,108,246); ink = disabled ? RGB(169,181,200) : RGB(255,255,255); border = fill; }
    else if (disabled) { fill = RGB(247,249,252); ink = RGB(177,188,205); border = RGB(233,238,246); }
    else if (selected) { fill = RGB(238,245,255); ink = RGB(35,94,222); border = RGB(179,205,255); }
    if (item.CtlID == SERVICE_LIST) InflateRect(&area, -app->px(4), -app->px(4));
    else InflateRect(&area, -1, -1);
    rounded(dc, area, fill, border, app->px(18));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ink);
    if (item.CtlID == SERVICE_LIST && item.itemID != static_cast<UINT>(-1)) {
        int count = static_cast<int>(SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0));
        std::wstring row(count + 1, L'\0'); SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(row.data())); row.resize(count);
        size_t split = row.find(L"  |  ");
        std::wstring name = row.substr(0, split), detail = split == std::wstring::npos ? L"" : row.substr(split + 5);
        size_t optional = name.find(L"（可选）");
        if (optional != std::wstring::npos) name.resize(optional);
        bool live = detail.find(L"进程运行中") != std::wstring::npos;
        size_t note = detail.find(L"（业务未验证）"); if (note != std::wstring::npos) detail.erase(note, 7);
        COLORREF dot = live ? RGB(31,158,112) : RGB(211,90,100);
        HBRUSH marker = CreateSolidBrush(dot); HGDIOBJ previous = SelectObject(dc, marker), pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, area.left + app->px(12), area.top + app->px(13), area.left + app->px(19), area.top + app->px(20));
        SelectObject(dc, previous); SelectObject(dc, pen); DeleteObject(marker);
        RECT text = {area.left + app->px(26), area.top + app->px(5), area.right - app->px(5), area.top + app->px(25)};
        SelectObject(dc, app->strong_font); DrawTextW(dc, name.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (live) {
            detail = L"运行中";
        }
        if (optional != std::wstring::npos) detail = L"可选 · " + detail;
        text.top += app->px(19); text.bottom += app->px(19); SelectObject(dc, app->font); SetTextColor(dc, RGB(134,149,171));
        DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else if (item.CtlID == LOG_SOURCE || item.CtlID == LOG_ENCODING) {
        FillRect(dc, &item.rcItem, app->surface);
        RECT row = item.rcItem; InflateRect(&row, -app->px(3), -app->px(2));
        if (selected) rounded(dc, row, RGB(238,245,255), RGB(238,245,255), app->px(10));
        SetTextColor(dc, selected ? RGB(35,94,222) : RGB(58,74,96));
        std::wstring value;
        if (item.itemID != static_cast<UINT>(-1)) {
            int length = static_cast<int>(SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0));
            if (length >= 0) { value.assign(length + 1, L'\0'); SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(value.data())); value.resize(length); }
        }
        RECT text = item.rcItem; InflateRect(&text, -app->px(12), 0);
        SelectObject(dc, app->font); DrawTextW(dc, value.c_str(), -1, &text,
                                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        return;
    } else if (item.CtlID != SERVICE_LIST) {
        SelectObject(dc, app->strong_font); std::wstring text = window_text(item.hwndItem);
        DrawTextW(dc, text.c_str(), -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (item.itemState & ODS_FOCUS) { InflateRect(&area, -3, -3); DrawFocusRect(dc, &area); }
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
        if (GetFocus() == window) { InflateRect(&area,-1,-1); DrawFocusRect(dc,&area); }
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
    wchar_t path[MAX_PATH];
    bool ok = SHGetPathFromIDListW(item, path);
    CoTaskMemFree(item);
    return ok ? path : L"";
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
            app->font = font(14, FW_NORMAL, L"Microsoft YaHei UI");
            app->title_font = font(22, FW_SEMIBOLD, L"Microsoft YaHei UI");
            app->strong_font = font(14, FW_SEMIBOLD, L"Microsoft YaHei UI");
            app->log_font = font(14, FW_NORMAL, L"Consolas");
            app->background = CreateSolidBrush(RGB(244,247,251)); app->surface = CreateSolidBrush(RGB(255,255,255));
            auto control = [&](const wchar_t *type, const wchar_t *text, DWORD style, int id) {
                bool checkbox = !std::wcscmp(type, L"BUTTON") && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
                if (!std::wcscmp(type, L"BUTTON") && !checkbox) style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
                HWND child = CreateWindowExW(0, type, text,
                    WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                require(child != nullptr, "Create native control");
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
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
            control(L"STATIC", L"0 / 0 运行中", SS_CENTERIMAGE, 213);
            control(L"STATIC", L"实例目录", 0, 201);
            app->path = control(L"EDIT", app->initial.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, INSTANCE);
            SendMessageW(app->path, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(app->px(8),app->px(6)));
            control(L"BUTTON", L"选择…", WS_TABSTOP, BROWSE);
            control(L"STATIC", L"MySQL 5.7.44", 0, 202);
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
            control(L"STATIC", L"服务控制", 0, 207);
            control(L"STATIC", L"服务 IP", 0, 216);
            app->service_ip = control(L"EDIT", L"127.0.0.1", ES_AUTOHSCROLL | WS_TABSTOP, SERVICE_IP);
            SendMessageW(app->service_ip, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(app->px(6), app->px(6)));
            control(L"BUTTON", L"应用", WS_TABSTOP, IP_APPLY);
            control(L"STATIC", L"全部停止后应用，下次启动生效", 0, 217);
            control(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_MULTICOLUMN | WS_TABSTOP, SERVICE_LIST);
            SendMessageW(GetDlgItem(window, SERVICE_LIST), LB_SETITEMHEIGHT, 0, app->px(54));
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
            for (int id : {202,207,209,214}) SendMessageW(GetDlgItem(window, id), WM_SETFONT, reinterpret_cast<WPARAM>(app->strong_font), TRUE);
            ShowWindow(GetDlgItem(window, 208), SW_HIDE);
            SendMessageW(app->output, EM_LIMITTEXT, 4 * 1024 * 1024, 0);
            app->load(); SetTimer(window, 1, 500, nullptr);
            return 0;
        }
        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) return 0;
            int width = MulDiv(LOWORD(lparam), 96, app->dpi), height = MulDiv(HIWORD(lparam), 96, app->dpi);
            int columns = app->grid_columns(width);
            app->grid_rows = std::max(2, (static_cast<int>(app->services.size())+columns-1)/columns);
            int db = app->database_top(), logs = db + 94;
            auto place = [&](int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(window, id), app->px(x), app->px(y), app->px(w), app->px(h), TRUE); };
            place(214,39,78,78,24); place(201,125,64,64,24);
            place(INSTANCE,202,124,width-710,26); place(BROWSE,width-500,119,64,36);
            place(FOLDER,width-424,119,90,36); place(LOGS,width-324,119,90,36);
            place(ALL_STOP,width-222,119,90,36); place(ALL_START,width-122,119,92,36);
            place(207,40,126,80,24); place(213,126,122,130,28);
            place(216,40,171,56,24); place(SERVICE_IP,108,167,142,26); place(IP_APPLY,266,162,66,36);
            place(217,346,171,300,24);
            place(DP2_ENABLE,width-630,122,96,26); place(FRIDA_ENABLE,width-520,122,80,26);
            place(PLUGINS_SAVE,width-430,119,80,32);
            place(ELF_START,width-336,119,96,32); place(ELF_STOP,width-230,119,96,32);
            place(SERVICE_LOGS,width-126,119,110,32);
            place(SERVICE_LIST,32,206,width-64,app->grid_rows*54);
            HWND services = GetDlgItem(window, SERVICE_LIST);
            SendMessageW(services, LB_SETCOLUMNWIDTH, (app->px(width-64))/columns, 0);
            LRESULT selected = SendMessageW(services, LB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < SendMessageW(services, LB_GETTOPINDEX, 0, 0))
                SendMessageW(services, LB_SETTOPINDEX, selected, 0);
            place(202,40,db+28,118,24); place(215,164,db+28,64,24); place(203,228,db+28,34,24); place(PORT,264,db+26,58,26);
            place(205,342,db+28,std::max(0,width-924),22);
            ShowWindow(GetDlgItem(window,205),width>=1240 ? SW_SHOW : SW_HIDE);
            for (int i = 0; i < 6; ++i) place(INITIALIZE+i,width-566+i*88,db+21,80,36);
            place(209,40,logs+23,100,24);
            place(LOG_SOURCE,width-804,logs+15,330,300); place(LOG_ENCODING,width-464,logs+15,132,180);
            place(LOG_REFRESH,width-322,logs+15,64,32); place(LOG_FOLDER,width-248,logs+15,84,32); place(LOG_FOLLOW,width-152,logs+17,112,26);
            place(210,40,logs+61,width-80,22); place(206,40,logs+94,width-80,height-logs-148);
            place(204,40,height-29,width-80,22);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {app->px(1120), app->px(820)}; return 0;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT *>(lparam)->itemHeight=app->px(32); return TRUE;
        case WM_DRAWITEM:
            draw_item(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam)); return TRUE;
        case WM_ERASEBKGND: {
            RECT area; GetClientRect(window, &area); FillRect(reinterpret_cast<HDC>(wparam), &area, app->background); return TRUE;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint);
            RECT area; GetClientRect(window, &area); int width = area.right, db = app->px(app->database_top()), logs = db+app->px(94);
            RECT footer = {0,area.bottom-app->px(38),width,area.bottom}; FillRect(dc,&footer,app->surface);
            auto card = [&](int top,int bottom) {
                RECT shadow = {app->px(24),top+app->px(3),width-app->px(24),bottom+app->px(3)};
                rounded(dc,shadow,RGB(233,237,245),RGB(233,237,245),app->px(24));
                RECT panel = {app->px(24),top,width-app->px(24),bottom};
                rounded(dc,panel,RGB(255,255,255),RGB(228,234,243),app->px(24));
            };
            card(app->px(20),app->px(84)); card(app->px(106),db-app->px(18)); card(db,db+app->px(76)); card(logs,area.bottom-app->px(54));
            RECT path_border = {app->px(198),app->px(118),width-app->px(432),app->px(156)};
            rounded(dc,path_border,RGB(255,255,255),RGB(225,233,246),app->px(16));
            RECT port_border = {app->px(260),db+app->px(22),app->px(326),db+app->px(54)};
            rounded(dc,port_border,RGB(255,255,255),RGB(225,233,246),app->px(14));
            RECT ip_border = {app->px(102),app->px(162),app->px(256),app->px(198)};
            rounded(dc,ip_border,RGB(255,255,255),RGB(225,233,246),app->px(16));
            HPEN line = CreatePen(PS_SOLID,1,RGB(239,243,249)); HGDIOBJ old=SelectObject(dc,line);
            for (int y : {app->px(84),app->px(157),logs+app->px(53),logs+app->px(88),static_cast<int>(area.bottom)-app->px(38)}) {
                MoveToEx(dc,app->px(24),y,nullptr);LineTo(dc,width-app->px(24),y);
            }
            SelectObject(dc,old);DeleteObject(line);EndPaint(window, &paint); return 0;
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
            SetWindowTextW(app->status,text->c_str()); return 0;
        }
        case STATUS_MESSAGE:
            SetWindowTextW(app->status, reinterpret_cast<const wchar_t *>(lparam)); return 0;
        case DONE_MESSAGE: {
            std::unique_ptr<Result> result(reinterpret_cast<Result *>(lparam));
            app->worker.join(); app->busy = false;
            if (result->stopped && app->session.process) { CloseHandle(app->session.process); app->session = {}; }
            if (result->started.process) app->session = result->started;
            if (result->created) { app->services = load_services(app->current, app->runtime); app->show_plugins(); }
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
                for (auto &service : app->services) if (refresh_service(service)) {
                    app->append(service.name + L"：" + service.state + L"\r\n诊断日志：" + service.err); changed = true;
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
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id == INSTANCE && HIWORD(wparam) == EN_CHANGE) { if (app->status && app->output) app->load(); return 0; }
            if (id == SERVICE_LIST && HIWORD(wparam) == LBN_SELCHANGE) { if (!app->busy) app->controls(); app->scan_logs(true); return 0; }
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
                auto chosen = choose_folder(window, L"选择本地实例目录", app->current);
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
                } else if (MessageBoxW(window, L"使用选定备份还原当前数据库？\n程序会先保存预备份，验证后再切换，并保留原数据冷快照。", L"确认还原", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                app->action(id, target);
            } else if (id == SERVICE_LOGS) {
                app->scan_logs(true);
            } else if (id >= ELF_START && id <= ALL_STOP)
                app->action(id, L"", static_cast<int>(SendMessageW(GetDlgItem(window, SERVICE_LIST), LB_GETCURSEL, 0, 0)));
            else if (id >= INITIALIZE && id <= RECOVER) app->action(id);
            return 0;
        }
        case WM_CLOSE:
            if (app->busy) { SetWindowTextW(app->status, L"正在执行操作，请等待完成后关闭"); return 0; }
            if (app->services_running()) { app->closing = true; app->action(ALL_STOP); return 0; }
            if (app->session.process) { app->closing = true; app->action(STOP); return 0; }
            DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            for (HFONT font : {app->font, app->title_font, app->strong_font, app->log_font}) DeleteObject(font);
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
        std::wstring folder(module); folder.resize(folder.find_last_of(L'\\'));
        state.runtime = folder + L"\\runtime";
        state.exe = state.runtime + L"\\mysql-control.exe";
        state.resources = folder + L"\\native";
        state.initial = folder + L"\\instances\\default";
        int count; wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
        require(args != nullptr, "Read application arguments");
        if (count == 3 && std::wstring(args[1]) == L"--instance") state.initial = full_path(args[2]);
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
