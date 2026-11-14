// Independent Win32 console for original ELF hosts and the native MySQL SFX.
#include "service_processes.h"
#include <algorithm>
#include <cwchar>
#include <memory>
#include <thread>

enum { INSTANCE = 101, BROWSE, PORT, INITIALIZE = 110, START, STOP, BACKUP, RESTORE, RECOVER, FOLDER, LOGS,
       SERVICE_LIST = 220, ELF_START, ELF_STOP, ALL_START, ALL_STOP, SERVICE_LOGS };
enum { STATUS_MESSAGE = WM_APP + 1, DONE_MESSAGE };

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
                          const std::vector<std::wstring> &arguments, const std::wstring &log) {
    directory(instance + L"\\.mysql");
    File output = private_file(log);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    File input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
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
                           const std::vector<std::wstring> &arguments, DWORD timeout = INFINITE) {
    std::wstring log = instance + L"\\.mysql\\" + unique_name() + L".private.log";
    HANDLE process = launch_tool(exe, instance, arguments, log);
    DWORD waited = WaitForSingleObject(process, timeout);
    if (waited != WAIT_OBJECT_0) {
        // This wrapper owns its official child through the SFX's Job. This
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

struct Result { bool success = false, stopped = false; Session started; std::wstring status, details; };

struct App {
    HWND window = nullptr, path = nullptr, port = nullptr, status = nullptr, output = nullptr;
    HFONT font = nullptr;
    std::wstring exe, initial, current;
    Session session;
    std::vector<Service> services;
    HANDLE service_job = nullptr;
    bool busy = false, closing = false;
    std::thread worker;

    bool services_running() const {
        return std::any_of(services.begin(), services.end(), [](const Service &service) { return running(service); });
    }
    void show_services() {
        HWND list = GetDlgItem(window, SERVICE_LIST);
        LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0); SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto &service : services) {
            std::wstring row = service.name + (service.automatic ? L"" : L"（可选）") + L"  |  " + service.state;
            if (service.pid) row += L"  |  PID " + std::to_wstring(service.pid);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
        if (!services.empty()) SendMessageW(list, LB_SETCURSEL, selected == LB_ERR ? 0 : selected, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
    }

    void append(const std::wstring &text) {
        SYSTEMTIME now; GetLocalTime(&now);
        wchar_t stamp[24]; std::swprintf(stamp, 24, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
        std::wstring line = stamp + text + L"\r\n";
        SendMessageW(output, EM_SETSEL, GetWindowTextLengthW(output), GetWindowTextLengthW(output));
        SendMessageW(output, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(output, EM_SCROLLCARET, 0, 0);
    }
    void controls() {
        // The worker may be changing process ownership. Inspect it after join.
        if (busy) {
            for (int id : {INSTANCE,BROWSE,PORT,INITIALIZE,START,STOP,BACKUP,RESTORE,RECOVER,FOLDER,LOGS,
                           SERVICE_LIST,ELF_START,ELF_STOP,ALL_START,ALL_STOP,SERVICE_LOGS})
                EnableWindow(GetDlgItem(window, id), FALSE);
            return;
        }
        bool active = services_running();
        bool initialized = !current.empty() && present(current + L"\\data\\auto.cnf");
        bool pending = !current.empty() && present(current + L"\\restore.pending");
        for (int id : {INSTANCE, BROWSE, PORT}) EnableWindow(GetDlgItem(window, id), !active && !session.process && (id != PORT || !initialized));
        EnableWindow(GetDlgItem(window, INITIALIZE), !busy && !session.process && !current.empty() && !initialized && !pending);
        EnableWindow(GetDlgItem(window, START), !busy && !session.process && initialized && !pending);
        EnableWindow(GetDlgItem(window, STOP), session.process && !active);
        for (int id : {BACKUP, RESTORE}) EnableWindow(GetDlgItem(window, id), !active && !session.process && initialized && !pending);
        EnableWindow(GetDlgItem(window, RECOVER), !busy && !session.process && pending);
        for (int id : {FOLDER, LOGS}) EnableWindow(GetDlgItem(window, id), !busy && !current.empty());
        LRESULT index = SendMessageW(GetDlgItem(window, SERVICE_LIST), LB_GETCURSEL, 0, 0);
        bool selected = index >= 0 && static_cast<size_t>(index) < services.size();
        EnableWindow(GetDlgItem(window, SERVICE_LIST), !services.empty());
        EnableWindow(GetDlgItem(window, ELF_START), selected && session.process && !running(services[index]) && !pending);
        EnableWindow(GetDlgItem(window, ELF_STOP), selected && running(services[index]));
        EnableWindow(GetDlgItem(window, SERVICE_LOGS), selected && !services[index].out.empty());
        EnableWindow(GetDlgItem(window, ALL_START), !services.empty() && initialized && !pending);
        EnableWindow(GetDlgItem(window, ALL_STOP), active || session.process);
    }
    void load() {
        if (busy || session.process || services_running()) return;
        std::wstring value = window_text(path);
        if (value.empty()) {
            current.clear(); services.clear(); show_services(); SetWindowTextW(status, L"请选择实例目录"); controls(); return;
        }
        current = full_path(value);
        services = load_services(current); show_services();
        bool initialized = present(current + L"\\data\\auto.cnf");
        if (present(current + L"\\server.cnf")) {
            std::string config = read_file(current + L"\\server.cnf");
            size_t at = config.find("\nport=");
            if (at != std::string::npos) SetWindowTextW(port, decode(config.substr(at + 6, config.find_first_of("\r\n", at + 6) - at - 6)).c_str());
        }
        SetWindowTextW(status, present(current + L"\\restore.pending") ? L"需要恢复未完成的还原" : initialized ? L"已选择实例，可启动数据库" : L"尚未初始化数据库");
        controls();
    }
    void action(int id, const std::wstring &target = L"", int selected = -1) {
        current = full_path(window_text(path));
        const std::wstring instance = current, tool = exe, port_text = window_text(port);
        const Session owned = session;
        busy = true; controls();
        SetWindowTextW(status, L"正在执行操作…");
        worker = std::thread([this, id, instance, tool, target, port_text, owned, selected] {
            auto result = std::make_unique<Result>();
            try {
                if (id == ALL_START || id == ALL_STOP || id == ELF_START || id == ELF_STOP) {
                    if (id == ALL_START && !owned.process) result->started = start_database(window, tool, instance);
                    if (id == ALL_START || id == ALL_STOP) {
                        if (id == ALL_START) {
                            for (auto &service : services) if (service.automatic) start_service(service, instance, service_job);
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
                    result->details = decode(command.output) + L"\r\n操作日志：" + command.log;
                    if (!target.empty()) result->details = L"备份目录：" + target + L"\r\n" + result->details;
                    if (command.code) throw std::runtime_error("Native database operation failed; inspect the operation log");
                    result->status = id == INITIALIZE ? L"安全初始化完成，数据库已停止" : id == BACKUP ? L"备份完成，数据库已停止" : id == RESTORE ? L"还原已验证并提交，数据库已停止" : L"恢复完成，数据库已停止";
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
            app->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
            auto control = [&](const wchar_t *type, const wchar_t *text, DWORD style, int id) {
                HWND child = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, text,
                    WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                require(child != nullptr, "Create native control");
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
                return child;
            };
            control(L"STATIC", L"实例目录", 0, 201);
            app->path = control(L"EDIT", app->initial.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, INSTANCE);
            control(L"BUTTON", L"选择…", WS_TABSTOP, BROWSE);
            control(L"STATIC", L"MySQL 5.7.44", 0, 202);
            control(L"STATIC", L"端口", 0, 203);
            app->port = control(L"EDIT", L"3306", ES_NUMBER | WS_TABSTOP, PORT);
            app->status = control(L"STATIC", L"选择实例目录以开始", SS_LEFT, 204);
            for (const auto &button : std::vector<std::pair<int, const wchar_t *>>{{INITIALIZE,L"初始化"},{START,L"启动"},{STOP,L"停止"},{BACKUP,L"备份…"},{RESTORE,L"还原…"},{RECOVER,L"恢复操作"},{FOLDER,L"实例目录"},{LOGS,L"日志目录"}})
                control(L"BUTTON", button.second, WS_TABSTOP, button.first);
            control(L"STATIC", L"备份与还原前请停止数据库。操作日志保存在实例目录中。", 0, 205);
            app->output = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 206);
            control(L"STATIC", L"原版服务进程（运行状态不代表业务就绪）", 0, 207);
            control(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, SERVICE_LIST);
            for (const auto &button : std::vector<std::pair<int, const wchar_t *>>{{ELF_START,L"启动所选"},{ELF_STOP,L"停止所选"},{ALL_START,L"整体启动"},{ALL_STOP,L"整体停止"},{SERVICE_LOGS,L"查看所选日志"}})
                control(L"BUTTON", button.second, WS_TABSTOP, button.first);
            SendMessageW(app->output, EM_LIMITTEXT, 4 * 1024 * 1024, 0);
            app->load(); SetTimer(window, 1, 500, nullptr);
            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(lparam), height = HIWORD(lparam);
            auto place = [&](int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(window, id), x, y, w, h, TRUE); };
            place(201,16,20,80,24); place(INSTANCE,100,16,width-212,30); place(BROWSE,width-100,16,84,30);
            place(202,16,64,135,26); place(203,165,64,45,26); place(PORT,212,60,80,28); place(204,310,64,width-326,26);
            for (int i = 0; i < 8; ++i) place(INITIALIZE+i,16+i*((width-32)/8),104,(width-32)/8-8,34);
            place(205,16,154,width-32,26); place(207,16,190,width-32,26);
            place(SERVICE_LIST,16,220,width-200,200);
            for (int i = 0; i < 5; ++i) place(ELF_START+i,width-172,220+i*40,156,32);
            place(206,16,436,width-32,height-452);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {880, 640}; return 0;
        case STATUS_MESSAGE:
            SetWindowTextW(app->status, reinterpret_cast<const wchar_t *>(lparam)); return 0;
        case DONE_MESSAGE: {
            std::unique_ptr<Result> result(reinterpret_cast<Result *>(lparam));
            app->worker.join(); app->busy = false;
            if (result->stopped && app->session.process) { CloseHandle(app->session.process); app->session = {}; }
            if (result->started.process) app->session = result->started;
            app->show_services(); SetWindowTextW(app->status, result->status.c_str()); app->append(result->details); app->controls();
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
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id == INSTANCE && HIWORD(wparam) == EN_CHANGE) { if (app->status && app->output) app->load(); return 0; }
            if (id == SERVICE_LIST && HIWORD(wparam) == LBN_SELCHANGE) { if (!app->busy) app->controls(); return 0; }
            if (HIWORD(wparam) != BN_CLICKED || app->busy) return 0;
            if (id == BROWSE) {
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
                LRESULT index = SendMessageW(GetDlgItem(window, SERVICE_LIST), LB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<size_t>(index) < app->services.size()) {
                    const auto &service = app->services[index];
                    app->append(service.name + L"\r\n" + service.out + L"\r\n" + decode(read_file(service.out)) + L"\r\n" + service.err + L"\r\n" + decode(read_file(service.err)));
                }
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
            KillTimer(window, 1); DeleteObject(app->font); PostQuitMessage(0); return 0;
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
        WSADATA sockets; require(WSAStartup(MAKEWORD(2,2), &sockets) == 0, "Initialize service networking");
        state.service_job = CreateJobObjectW(nullptr, nullptr); require(state.service_job != nullptr, "Create ELF host job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(state.service_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Configure ELF host ownership");
        wchar_t module[32768]; require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Find native application directory");
        std::wstring folder(module); folder.resize(folder.find_last_of(L'\\'));
        state.exe = folder + L"\\mysql-standalone.exe";
        state.initial = folder + L"\\instances\\default";
        int count; wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
        require(args != nullptr, "Read application arguments");
        if (count == 3 && std::wstring(args[1]) == L"--instance") state.initial = full_path(args[2]);
        else if (count != 1) { LocalFree(args); throw std::runtime_error("Usage: DNF-Native.exe [--instance DIRECTORY]"); }
        LocalFree(args);
        WNDCLASSW type = {}; type.hInstance = instance; type.lpfnWndProc = window_proc; type.lpszClassName = L"DNFNativeController";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        require(RegisterClassW(&type) != 0, "Register native window");
        HWND window = CreateWindowExW(0, type.lpszClassName, L"DNF Native 控制台", WS_OVERLAPPEDWINDOW,
                                       CW_USEDEFAULT,CW_USEDEFAULT,1040,800,nullptr,nullptr,instance,nullptr);
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
