// Native ownership and Linux stop events for this project's ELF hosts.
#pragma once
#include <winsock2.h>
#include "../win32_files.h"
#include "../game_levels.h"
#include "log_view.h"
#include <algorithm>
#include <cstdint>

static const wchar_t *dp2_preload = L"LD_PRELOAD=/dp2/libdp2pre.so";

static bool robot_profile_member(const std::wstring &name) {
    return name == L"game" || name == L"auction" || name == L"point" || name == L"monitor" || name == L"relay";
}

struct Service {
    std::wstring name, host, root, cwd, elf, loader_hash, elf_hash, pid_file;
    std::wstring executable, config_dir, stop_event, robot_pid_file;
    std::vector<std::wstring> arguments, environment;
    bool automatic = false, external = false;
    unsigned max_level = 70;
    unsigned signal = 15;
    HANDLE process = nullptr;
    DWORD pid = 0;
    bool stopping = false;
    bool normal_stop = false;
    HANDLE stop_handle = nullptr;
    std::wstring state = L"未启动", out, err;
    // Current launch status, updated from its runtime channel and fresh logs.
    std::wstring readiness = L"未启动";
    bool application_initialized = false;
    uint64_t readiness_offset = 0;
    std::string readiness_tail;
    struct InitLog { std::wstring path; uint64_t offset; std::string tail; };
    std::vector<InitLog> readiness_init;
};

static std::wstring setting(const std::wstring &file, const std::wstring &section, const std::wstring &key) {
    wchar_t value[32768];
    DWORD size = GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", value, 32768, file.c_str());
    if (size >= 32767) throw std::runtime_error("Service setting is too long");
    return std::wstring(value, size);
}

#include "game_keys.h"

static std::string utf8_path(const std::wstring &value) {
    if (value.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::runtime_error("Encode Robot path");
    std::string result(bytes, '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr) == bytes,
            "Encode Robot path");
    return result;
}

static std::string json_escape(const std::string &value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(static_cast<char>(c)); break;
        }
    }
    return result;
}

static std::vector<Service> load_services(const std::wstring &instance, const std::wstring &runtime) {
    std::wstring file = instance + L"\\services.ini";
    std::vector<Service> services;
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return services;
    if (setting(file, L"services", L"version") != L"1") throw std::runtime_error("Unsupported service instance format");
    std::wstring plugin_mode = setting(file, L"plugins", L"mode");
    std::wstring maximum = setting(file, L"gameplay", L"max_level");
    unsigned maximum_level = 70;
    if (!maximum.empty()) {
        size_t used = 0;
        maximum_level = std::stoul(maximum, &used);
        if (used != maximum.size() || !game_level_valid(maximum_level)) throw std::runtime_error("Invalid game maximum level");
    }
    if (!plugin_mode.empty() && plugin_mode != L"off" && plugin_mode != L"lua" && plugin_mode != L"frida")
        throw std::runtime_error("Unknown game plugin mode");
    unsigned count = GetPrivateProfileIntW(L"services", L"count", 0, file.c_str());
    if (!count || count > 19) throw std::runtime_error("Invalid service count");
    for (unsigned i = 0; i < count; ++i) {
        Service service;
        service.name = setting(file, L"services", L"service" + std::to_wstring(i));
        if (service.name.empty()) throw std::runtime_error("Service name is missing");
        auto get = [&](const wchar_t *key) { return setting(file, service.name, key); };
        service.host = runtime + L"\\" + get(L"host");
        service.root = instance + L"\\" + get(L"root");
        service.cwd = get(L"cwd");
        service.elf = service.root + get(L"elf");
        service.elf_hash = get(L"elf_sha256"); service.loader_hash = get(L"loader_sha256");
        auto pid = get(L"pid_file");
        if (!pid.empty()) service.pid_file = service.root + pid;
        if (robot_profile_member(service.name))
            service.robot_pid_file = instance + L"\\robot\\state\\pids\\" + service.name + L".pid";
        if (service.cwd.empty() || service.elf_hash.size() != 64 || service.loader_hash.size() != 64)
            throw std::runtime_error("Service paths or ELF identities are missing");
        service.automatic = get(L"automatic") == L"1";
        service.signal = get(L"stop_signal") == L"12" ? 12 : 15;
        for (unsigned j = 0; ; ++j) {
            auto arg = get((L"arg" + std::to_wstring(j)).c_str());
            if (arg.empty()) break;
            service.arguments.push_back(arg);
        }
        for (unsigned j = 0; ; ++j) {
            auto env = get((L"env" + std::to_wstring(j)).c_str());
            if (env.empty()) break;
            service.environment.push_back(env);
        }
        if (service.name == L"game" && !plugin_mode.empty()) {
            auto &env = service.environment;
            env.erase(std::remove(env.begin(), env.end(), dp2_preload), env.end());
            env.erase(std::remove_if(env.begin(), env.end(), [](const std::wstring &value) {
                return value.rfind(L"DNF_NATIVE_FRIDA=", 0) == 0;
            }), env.end());
            if (plugin_mode != L"off") env.push_back(dp2_preload);
            env.push_back(plugin_mode == L"frida" ? L"DNF_NATIVE_FRIDA=1" : L"DNF_NATIVE_FRIDA=0");
        }
        if (service.name == L"game") {
            service.max_level = maximum_level;
            auto &env = service.environment;
            env.erase(std::remove_if(env.begin(), env.end(), [](const std::wstring &value) {
                return value.rfind(L"DNF_NATIVE_MAX_LEVEL=", 0) == 0;
            }), env.end());
            env.push_back(L"DNF_NATIVE_MAX_LEVEL=" + std::to_wstring(maximum_level));
        }
        if (service.name == L"auction" || service.name == L"point") {
            auto &env=service.environment;
            env.erase(std::remove_if(env.begin(),env.end(),[](const std::wstring &s) {return s.rfind(L"DNF_NATIVE_AUCTION_",0)==0;}),env.end());
            unsigned level=GetPrivateProfileIntW(L"auction",L"level_limit",70,file.c_str());
            unsigned refine=GetPrivateProfileIntW(L"auction",L"refine_limit",7,file.c_str());
            if(GetPrivateProfileIntW(L"robot",L"enabled",0,file.c_str())==1) {level=127;refine=31;}
            if(!level || level>127 || refine>31) throw std::runtime_error("Invalid auction runtime limits");
            env.push_back(L"DNF_NATIVE_AUCTION_LEVEL="+std::to_wstring(level));
            env.push_back(L"DNF_NATIVE_AUCTION_REFINE="+std::to_wstring(refine));
            // Inspect the consumed index in the host at every launch, including
            // launches made directly by Robot after rebuilding its Big5 index.
            env.push_back(L"DNF_NATIVE_AUCTION_ENCODING=AUTO");
            if(setting(file,L"auction",L"price_keys")==L"1" || setting(file,L"auction",L"price_keys_"+service.name)==L"1") env.push_back(L"DNF_NATIVE_AUCTION_PRICE_KEYS=1");
        }
        services.push_back(service);
    }
    if (GetPrivateProfileIntW(L"robot", L"enabled", 0, file.c_str()) == 1) {
        Service robot;
        robot.name = L"robot";
        robot.external = true;
        robot.automatic = true;
        robot.executable = setting(file, L"robot", L"executable");
        if (robot.executable.empty()) robot.executable = runtime + L"\\robot.exe";
        if (robot.executable.find(L':') == std::wstring::npos && robot.executable.rfind(L"\\", 0) != 0)
            robot.executable = instance + L"\\" + robot.executable;
        robot.config_dir = setting(file, L"robot", L"config_dir");
        if (robot.config_dir.empty()) robot.config_dir = instance + L"\\robot";
        if (robot.config_dir.find(L':') == std::wstring::npos && robot.config_dir.rfind(L"\\", 0) != 0)
            robot.config_dir = instance + L"\\" + robot.config_dir;
        robot.cwd = instance;
        // Keep the runtime root on the service object so the profile writer
        // can reconstruct the complete Native launch contract when Robot is
        // started before any market service.
        robot.host = runtime;
        robot.state = L"未启动";
        robot.readiness = L"未启动";
        services.push_back(robot);
    }
    return services;
}

static bool running(const Service &service) {
    return service.process && WaitForSingleObject(service.process, 0) == WAIT_TIMEOUT;
}

static std::string robot_profile_string(const std::wstring &value) {
    return "\"" + json_escape(utf8_path(value)) + "\"";
}

static std::vector<std::wstring> service_start_arguments(const Service &service) {
    std::vector<std::wstring> args{service.host, L"--root", service.root, service.loader_hash};
    for (const auto &env : service.environment) { args.push_back(L"--env"); args.push_back(env); }
    args.push_back(L"--cwd"); args.push_back(service.cwd);
    args.push_back(L"--run"); args.push_back(service.elf); args.push_back(service.elf_hash);
    args.insert(args.end(), service.arguments.begin(), service.arguments.end());
    return args;
}

static std::string robot_profile_service_json(const Service &service) {
    std::string result = "{\"executable\":" + robot_profile_string(service.host) +
        ",\"image\":" + robot_profile_string(service.elf) +
        ",\"image_sha256\":" + robot_profile_string(service.elf_hash) +
        ",\"pid_file\":" + robot_profile_string(service.robot_pid_file) +
        ",\"directory\":" + robot_profile_string(service.root + service.cwd) +
        ",\"stop_event\":\"Local\\\\dnf-native-" + std::string(service.signal==12?"sigusr2":"terminate") + "-{pid}\",\"start\":[";
    bool first = true;
    for (const auto &arg : service_start_arguments(service)) {
        if (!first) result += ',';
        first = false;
        result += robot_profile_string(arg);
    }
    return result + "]}";
}

static void write_robot_profile(const Service &robot, const std::wstring &instance) {
    // This file describes the launcher-owned host contract. Robot owns its
    // other configuration and runtime data; Native refreshes only this file.
    std::string json = "{\n  \"version\":1,\"adapter\":\"dnf-native-windows\",\n"
        "  \"market_owner\":\"robot\",\"memory_model\":\"refine-banks-31\",\n"
        "  \"service_root\":" + robot_profile_string(instance + L"\\home\\neople") +
        ",\n  \"capabilities\":{\"auction\":true,\"point\":true,\"memory_patch\":true},\n  \"services\":{\n";
    bool first = true;
    for (const auto &service : load_services(instance, robot.host)) {
        if (!robot_profile_member(service.name)) continue;
        if (!first) json += ",\n";
        first = false;
        json += "    " + robot_profile_string(service.name) + ":" + robot_profile_service_json(service);
    }
    write_private(robot.config_dir + L"\\conf\\host-profile.json", json + "\n  }\n}\n",CREATE_ALWAYS);
}

static std::vector<LogSource> game_initialization_logs(const Service &service) {
    std::vector<LogSource> logs;
    if (service.name == L"game") application_logs(service.root + service.cwd + L"\\log", L"", logs);
    logs.erase(std::remove_if(logs.begin(), logs.end(), [](const LogSource &log) {
        return log.path.size() < 5 || log.path.substr(log.path.size() - 5) != L".init";
    }), logs.end());
    return logs;
}

static void reset_service_readiness(Service &service) {
    service.application_initialized = false;
    service.readiness_offset = 0;
    service.readiness_tail.clear();
    service.readiness_init.clear();
    // Snapshot old application logs before starting the process. Only bytes
    // appended by this launch may establish readiness, including after midnight.
    for (const auto &log : game_initialization_logs(service)) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(log.path.c_str(), GetFileExInfoStandard, &data))
            service.readiness_init.push_back({log.path,
                (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow, {}});
    }
}

static bool read_initialization_marker(const std::wstring &path, uint64_t &position, std::string &tail) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && uint64_t(size.QuadPart) < position) { position = 0; tail.clear(); }
    LARGE_INTEGER offset; offset.QuadPart = static_cast<LONGLONG>(position);
    bool found = false;
    if (SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        char bytes[16384];
        DWORD count = 0;
        if (ReadFile(file, bytes, sizeof(bytes), &count, nullptr) && count) {
            position += count;
            tail.append(bytes, count);
            const std::string marker = " server has been started successfully.";
            found = tail.find(marker) != std::string::npos;
            if (tail.size() >= marker.size()) tail.erase(0, tail.size() - marker.size() + 1);
        }
    }
    CloseHandle(file);
    return found;
}

static bool refresh_service_readiness(Service &service) {
    if (service.name != L"game" || service.application_initialized || !running(service) || service.out.empty()) return false;
    bool found = read_initialization_marker(service.out, service.readiness_offset, service.readiness_tail);
    // Original stdout may stay buffered indefinitely on an idle server. Its
    // native .init log flushes the same success record during initialization.
    for (const auto &log : game_initialization_logs(service)) {
        auto item = std::find_if(service.readiness_init.begin(), service.readiness_init.end(),
            [&](const Service::InitLog &entry) { return entry.path == log.path; });
        if (item == service.readiness_init.end()) {
            service.readiness_init.push_back({log.path, 0, {}});
            item = service.readiness_init.end() - 1;
        }
        found = read_initialization_marker(item->path, item->offset, item->tail) || found;
    }
    if (!found) return false;
    service.application_initialized = true;
    service.state = L"进程运行中（应用已初始化）";
    service.readiness = L"应用已初始化";
    return true;
}

static bool refresh_service(Service &service) {
    if (!service.process || running(service)) return false;
    DWORD code = 0;
    require(GetExitCodeProcess(service.process, &code), "Read service process exit code");
    // Original CApplication leaves its PID marker after a normal foreground
    // shutdown. Remove only the marker identifying this now-exited own host.
    if (!service.pid_file.empty() && GetFileAttributesW(service.pid_file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string text = read_file(service.pid_file);
        while (!text.empty() && std::strchr("\r\n \t", text.back())) text.pop_back();
        if (text == std::to_string(service.pid)) require(DeleteFileW(service.pid_file.c_str()), "Remove exited owned service PID marker");
    }
    if (!service.robot_pid_file.empty() && GetFileAttributesW(service.robot_pid_file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string marker=read_file(service.robot_pid_file);
        if(marker==std::to_string(service.pid)) require(DeleteFileW(service.robot_pid_file.c_str()), "Remove Robot host PID marker");
    }
    // A requested stop is reported as such, but the exit code stays visible so a
    // crash during shutdown cannot hide behind the word "已停止".
    bool expected_stop = service.stopping;
    bool original_code_one=service.name==L"manager" || service.name==L"dbmw_guild" ||
        service.name==L"dbmw_mnt" || service.name==L"dbmw_stat" || service.name==L"monitor" ||
        service.name==L"guild" || service.name==L"statics" || service.name==L"coserver";
    service.normal_stop=expected_stop && (code==0 || (code==1 && original_code_one &&
        !service.out.empty() && read_file(service.out).find("Application Stop!")!=std::string::npos));
    service.state = (expected_stop ? L"已停止，代码 " : L"已退出，代码 ") + std::to_wstring(code);
    service.readiness = L"宿主已退出";
    service.stopping = false;
    if (service.stop_handle) { CloseHandle(service.stop_handle); service.stop_handle = nullptr; }
    CloseHandle(service.process); service.process = nullptr;
    return true;
}

// Robot may restart a market host. Its children inherit our job, and its PID
// marker lets the controller reconnect normal process/log/resource controls.
static bool adopt_robot_service(Service &service, HANDLE job, const std::wstring &robot_logs) {
    if(running(service) || service.robot_pid_file.empty() ||
       GetFileAttributesW(service.robot_pid_file.c_str())==INVALID_FILE_ATTRIBUTES) return false;
    std::string text=read_file(service.robot_pid_file);
    if(text.empty() || text.size()>10 || text.find_first_not_of("0123456789")!=std::string::npos) return false;
    DWORD pid=static_cast<DWORD>(std::stoul(text));
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);
    if(!process) return false;
    BOOL owned=FALSE;wchar_t image[32768];DWORD size=32768;
    bool valid=IsProcessInJob(process,job,&owned) && owned &&
        QueryFullProcessImageNameW(process,0,image,&size) &&
        !_wcsicmp(instance_directory(image).c_str(),instance_directory(service.host).c_str()) &&
        WaitForSingleObject(process,0)==WAIT_TIMEOUT;
    if(!valid) {CloseHandle(process);return false;}
    if(service.process) CloseHandle(service.process);
    service.process=process;service.pid=pid;service.stopping=false;service.normal_stop=false;
    service.state=L"进程运行中";service.readiness=L"Robot 已重新拉起宿主";
    service.application_initialized=false;service.readiness_offset=0;service.readiness_tail.clear();
    service.out=robot_logs+L"\\"+service.name+L"-host.log";service.err=service.out;
    return true;
}

#include "game_compat.h"

static void start_service(Service &service, const std::wstring &instance, HANDLE job) try {
    refresh_service(service);
    if (service.process) return;
    service.stopping = false;
    service.normal_stop = false;
    if (service.external) {
        prepare_game_keys(instance);
        directory(service.config_dir + L"\\conf");
        const std::wstring config = service.config_dir + L"\\conf\\config.ini";
        if (GetFileAttributesW(config.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string text = "[Ports]\nRobotAPI = 8111\nWeb = 8112\nGame = 10011\nMonitor = 30303\nAuction = 30803\nPoint = 30603\nRelay = 7200\nPartyRoute0 = 5063\n\n[Robot]\nDfGameR = " + utf8_path(instance + L"\\home\\neople\\game\\df_game_r") + "\nRobotInnerIp = 127.0.0.1\nRobotConnectIp = 127.0.0.1\nGameServerGroup = 3\n\n[Services]\nRoot = " + utf8_path(instance + L"\\home\\neople") + "\nRunScript = " + utf8_path(instance + L"\\robot\\run.bat") + "\nAuctionHost = 127.0.0.1\nPointHost = 127.0.0.1\nRelayHost = 127.0.0.1\n\n[Web]\nWebPassword = twadmin\n\n[db]\ndb_host = 127.0.0.1\ndb_user_name = game\ndb_password = uu5!^%jg\ndb_database_name = d_taiwan\ndb_port = 3306\n\n[system]\nlog_max_size_mb = 100\nlog_max_backups = 5\nmax_response_bytes = 4194304\n";
            write_private(config, text);
        }
        const std::wstring profile = service.config_dir + L"\\conf\\host-profile.json";
        if (GetFileAttributesW(profile.c_str()) == INVALID_FILE_ATTRIBUTES ||
            read_file(profile).find("\"adapter\":\"dnf-native-windows\"") != std::string::npos ||
            read_file(profile).find("\"adapter\": \"dnf-native-windows\"") != std::string::npos)
            write_robot_profile(service, instance);
        service.state = L"正在启动"; service.readiness = L"Robot 启动中";
    } else {
        prepare_game_compat(service);
    }
    service.state = L"正在启动";
    service.readiness = L"宿主启动中";
    reset_service_readiness(service);
    directory(instance + L"\\service-logs");
    std::wstring tag = service.name + L"-" + std::to_wstring(GetTickCount64());
    service.out = instance + L"\\service-logs\\" + tag + L".stdout.private.log";
    service.err = instance + L"\\service-logs\\" + tag + L".stderr.private.log";
    File output = private_file(service.out), error = private_file(service.err);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    File input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    std::wstring command, ready_name;
    if (service.external) {
        service.stop_event = L"Local\\dnf-native-robot-stop-" + std::to_wstring(GetTickCount64());
        service.stop_handle = CreateEventW(nullptr, TRUE, FALSE, service.stop_event.c_str());
        require(service.stop_handle != nullptr, "Create Robot stop event");
        command = quote(service.executable) + L" --config-dir " + quote(service.config_dir) + L" --stop-event " + quote(service.stop_event);
        ready_name=service.stop_event+L"-ready";
    } else for(const auto &arg:service_start_arguments(service)) {
        if(!command.empty()) command+=L" ";
        command+=quote(arg);
    }
    File ready_event(CreateEventW(nullptr,TRUE,FALSE,ready_name.empty()?nullptr:ready_name.c_str()));
    if (service.external) command+=L" --ready-event "+quote(ready_name);
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.handle; startup.hStdOutput = output.handle; startup.hStdError = error.handle;
    PROCESS_INFORMATION child = {};
    const std::wstring &program = service.external ? service.executable : service.host;
    require(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
                           DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, instance.c_str(), &startup, &child), "Start ELF host");
    if (!AssignProcessToJobObject(job, child.hProcess) || ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
        DWORD error_code = GetLastError();
        TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 10000);
        CloseHandle(child.hThread); CloseHandle(child.hProcess); SetLastError(error_code);
        require(false, "Own ELF host before execution");
    }
    CloseHandle(child.hThread); service.process = child.hProcess; service.pid = child.dwProcessId;
    if (!service.external && robot_profile_member(service.name)) {
        service.robot_pid_file=instance+L"\\robot\\state\\pids\\"+service.name+L".pid";
        directory(instance+L"\\robot\\state\\pids");
        write_private(service.robot_pid_file,std::to_string(service.pid),CREATE_ALWAYS);
    }
    if (service.external) {
        ULONGLONG deadline = GetTickCount64() + 120000;
        for (;;) {
            if (refresh_service(service)) throw std::runtime_error("Robot exited during startup; inspect service logs");
            if (WaitForSingleObject(ready_event.handle,0)==WAIT_OBJECT_0) {
                service.application_initialized=true;
                service.state = L"进程运行中"; service.readiness = L"Robot 已初始化"; return;
            }
            if (GetTickCount64() >= deadline) throw std::runtime_error("Robot startup was not confirmed; inspect service logs");
            Sleep(50);
        }
    }
    // The event only confirms that our runtime installed its signal channel.
    // It does not prove that the original application completed initialization.
    ULONGLONG deadline = GetTickCount64() + 10000;
    for (;;) {
        if (refresh_service(service)) throw std::runtime_error("ELF host exited during startup; inspect service logs");
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, (L"Local\\dnf-native-terminate-" + std::to_wstring(service.pid)).c_str());
        if (event) {
            CloseHandle(event);
            service.state = L"进程运行中";
            service.readiness = service.name == L"game" ? L"运行时通道已接入，等待应用初始化" :
                                                        L"运行时通道已接入";
            return;
        }
        if (GetTickCount64() >= deadline) throw std::runtime_error("ELF host signal channel timed out; process retained for diagnosis");
        Sleep(25);
    }
} catch (...) {
    service.state = running(service) ? L"启动未确认，进程保留" : L"启动失败";
    service.readiness = running(service) ? L"启动超时（进程保留，查看诊断）" : L"启动失败（查看诊断）";
    throw;
}

static void stop_service(Service &service) {
    if (refresh_service(service) || !service.process) return;
    if (service.external) {
        if (!service.stop_handle || !SetEvent(service.stop_handle)) throw std::runtime_error("Request Robot shutdown");
        service.stopping = true; service.state = L"正在停止"; service.readiness = L"停止请求已发送";
        if (WaitForSingleObject(service.process, 30000) != WAIT_OBJECT_0) {
            service.state = L"停止超时，进程保留"; service.readiness = L"停止超时（进程保留）";
            throw std::runtime_error("Robot stop timed out; process retained");
        }
        refresh_service(service); return;
    }
    std::wstring prefix = service.signal == 12 ? L"Local\\dnf-native-sigusr2-" : L"Local\\dnf-native-terminate-";
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + std::to_wstring(service.pid)).c_str());
    require(event != nullptr, "Open owned ELF host stop event");
    bool ok = SetEvent(event); CloseHandle(event); require(ok, "Request original service shutdown");
    service.stopping = true;
    service.state = L"正在停止";
    service.readiness = L"停止请求已发送";
    if (service.name == L"stun") {
        SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) throw std::runtime_error("Create STUN receive wake socket");
        sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_port = htons(2311);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int sent = sendto(socket, "\1", 1, 0, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        closesocket(socket);
        if (sent != 1) throw std::runtime_error("Wake original STUN receive failed");
    }
    if (WaitForSingleObject(service.process, service.name == L"game" ? 180000 : 30000) != WAIT_OBJECT_0) {
        service.state = L"停止超时，进程保留";
        service.readiness = L"停止超时（进程保留）";
        throw std::runtime_error("Service stop timed out; dependent services and database retained");
    }
    refresh_service(service);
}
