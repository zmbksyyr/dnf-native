// Native ownership and Linux stop events for this project's ELF hosts.
#pragma once
#include <winsock2.h>
#include "../win32_files.h"
#include "../game_levels.h"
#include <algorithm>
#include <cstdint>

static const wchar_t *dp2_preload = L"LD_PRELOAD=/dp2/libdp2pre.so";

struct Service {
    std::wstring name, host, root, cwd, elf, loader_hash, elf_hash, pid_file;
    std::vector<std::wstring> arguments, environment;
    bool automatic = false;
    unsigned max_level = 70;
    unsigned signal = 15;
    HANDLE process = nullptr;
    DWORD pid = 0;
    bool stopping = false;
    bool normal_stop = false;
    std::wstring state = L"未启动", out, err;
    // Current launch status, updated from its runtime channel and fresh logs.
    std::wstring readiness = L"未启动";
    bool application_initialized = false;
    uint64_t readiness_offset = 0;
    std::string readiness_tail;
};

static std::wstring setting(const std::wstring &file, const std::wstring &section, const std::wstring &key) {
    wchar_t value[32768];
    DWORD size = GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", value, 32768, file.c_str());
    if (size >= 32767) throw std::runtime_error("Service setting is too long");
    return std::wstring(value, size);
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
            if(!level || level>127 || refine>31) throw std::runtime_error("Invalid auction runtime limits");
            env.push_back(L"DNF_NATIVE_AUCTION_LEVEL="+std::to_wstring(level));
            env.push_back(L"DNF_NATIVE_AUCTION_REFINE="+std::to_wstring(refine));
            if(setting(instance+L"\\.mysql\\market-index.ini",L"index",L"encoding")==L"UTF8")
                env.push_back(L"DNF_NATIVE_AUCTION_ENCODING=UTF8");
            if(setting(file,L"auction",L"price_keys")==L"1" || setting(file,L"auction",L"price_keys_"+service.name)==L"1") env.push_back(L"DNF_NATIVE_AUCTION_PRICE_KEYS=1");
        }
        services.push_back(service);
    }
    return services;
}

static bool running(const Service &service) {
    return service.process && WaitForSingleObject(service.process, 0) == WAIT_TIMEOUT;
}

static bool refresh_service_readiness(Service &service) {
    // Only game has an original, verified initialization marker. Read the
    // fresh stdout belonging to this launch, never the application's old
    // .init files or a previous host's output.
    if (service.name != L"game" || service.application_initialized || !running(service) || service.out.empty()) return false;
    HANDLE file = CreateFileW(service.out.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER offset; offset.QuadPart = static_cast<LONGLONG>(service.readiness_offset);
    bool changed = false;
    if (SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        char bytes[16384];
        DWORD count = 0;
        if (ReadFile(file, bytes, sizeof(bytes), &count, nullptr) && count) {
            service.readiness_offset += count;
            service.readiness_tail.append(bytes, count);
            const std::string marker = " server has been started successfully.";
            if (service.readiness_tail.find(marker) != std::string::npos) {
                service.application_initialized = true;
                service.state = L"进程运行中（应用已初始化）";
                service.readiness = L"应用已初始化";
                changed = true;
            }
            // Retain only the overlap needed if a read splits the marker.
            if (service.readiness_tail.size() >= marker.size())
                service.readiness_tail.erase(0, service.readiness_tail.size() - marker.size() + 1);
        }
    }
    CloseHandle(file);
    return changed;
}

static bool refresh_service(Service &service) {
    if (!service.process || running(service)) return false;
    DWORD code = 0;
    require(GetExitCodeProcess(service.process, &code), "Read ELF host exit code");
    // Original CApplication leaves its PID marker after a normal foreground
    // shutdown. Remove only the marker identifying this now-exited own host.
    if (!service.pid_file.empty() && GetFileAttributesW(service.pid_file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string text = read_file(service.pid_file);
        while (!text.empty() && std::strchr("\r\n \t", text.back())) text.pop_back();
        if (text == std::to_string(service.pid)) require(DeleteFileW(service.pid_file.c_str()), "Remove exited owned service PID marker");
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
    CloseHandle(service.process); service.process = nullptr;
    return true;
}

#include "game_compat.h"

static void start_service(Service &service, const std::wstring &instance, HANDLE job) try {
    refresh_service(service);
    if (service.process) return;
    service.stopping = false;
    service.normal_stop = false;
    prepare_game_compat(service);
    service.state = L"正在启动";
    service.readiness = L"宿主启动中";
    service.application_initialized = false;
    service.readiness_offset = 0;
    service.readiness_tail.clear();
    directory(instance + L"\\service-logs");
    std::wstring tag = service.name + L"-" + std::to_wstring(GetTickCount64());
    service.out = instance + L"\\service-logs\\" + tag + L".stdout.private.log";
    service.err = instance + L"\\service-logs\\" + tag + L".stderr.private.log";
    File output = private_file(service.out), error = private_file(service.err);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    File input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    std::wstring command = quote(service.host) + L" --root " + quote(service.root) + L" " + quote(service.loader_hash);
    for (const auto &env : service.environment) command += L" --env " + quote(env);
    command += L" --cwd " + quote(service.cwd) + L" --run " + quote(service.elf) + L" " + quote(service.elf_hash);
    for (const auto &arg : service.arguments) command += L" " + quote(arg);
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.handle; startup.hStdOutput = output.handle; startup.hStdError = error.handle;
    PROCESS_INFORMATION child = {};
    require(CreateProcessW(service.host.c_str(), command.data(), nullptr, nullptr, TRUE,
                           DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, instance.c_str(), &startup, &child), "Start ELF host");
    if (!AssignProcessToJobObject(job, child.hProcess) || ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
        DWORD error_code = GetLastError();
        TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 10000);
        CloseHandle(child.hThread); CloseHandle(child.hProcess); SetLastError(error_code);
        require(false, "Own ELF host before execution");
    }
    CloseHandle(child.hThread); service.process = child.hProcess; service.pid = child.dwProcessId;
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
