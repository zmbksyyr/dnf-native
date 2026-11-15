// Shared Windows file, permission and argument handling for native delivery tools.
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

inline void require(bool ok, const char *operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " (Win32 " + std::to_string(GetLastError()) + ")");
}

inline std::wstring instance_directory(const std::wstring &value) {
    if (value.empty()) throw std::runtime_error("Choose an instance directory first");
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    require(size && size < buffer.size(), "Resolve instance path");
    std::wstring path(buffer.data(), size);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    if (path.compare(0, 4, L"\\\\?\\") == 0) {
        path = path.compare(4, 4, L"UNC\\") == 0 ? L"\\\\" + path.substr(8) : path.substr(4);
    }
    if (path.size() < MAX_PATH) return path;
    // The official MySQL 5.7 process cannot use an extended-length working
    // directory. An existing filesystem short name still refers to the same
    // directory, so locks, database identity and cold copies remain in place.
    std::wstring extended = path.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
    size = GetShortPathNameW(extended.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size && size < buffer.size()) {
        std::wstring short_path(buffer.data(), size);
        if (short_path.compare(0, 8, L"\\\\?\\UNC\\") == 0) short_path = L"\\\\" + short_path.substr(8);
        else if (short_path.compare(0, 4, L"\\\\?\\") == 0) short_path.erase(0, 4);
        if (short_path.size() < MAX_PATH) return short_path;
    }
    throw std::runtime_error("Instance path exceeds the official MySQL working-directory limit and has no usable short name; choose a shorter directory");
}

inline void directory(const std::wstring &path) {
    int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        throw std::runtime_error("Create directory failed (Win32 " + std::to_string(result) + ")");
}

inline std::wstring quote(const std::wstring &argument) {
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

class File {
public:
    HANDLE handle;
    explicit File(HANDLE value) : handle(value) { require(value != INVALID_HANDLE_VALUE, "Open instance file"); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

inline PSECURITY_DESCRIPTOR private_security() {
    HANDLE token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "Read current Windows user");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    bool ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    require(ok, "Read current user SID");
    wchar_t *sid = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid), "Format current user SID");
    std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr),
            "Build private instance permissions");
    return descriptor;
}

inline File private_file(const std::wstring &path, DWORD disposition = CREATE_NEW) {
    PSECURITY_DESCRIPTOR descriptor = private_security();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, TRUE};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    return File(file);
}

inline void write_private(const std::wstring &path, const std::string &text, DWORD disposition = CREATE_NEW) {
    File file = private_file(path, disposition);
    DWORD bytes = 0;
    require(WriteFile(file.handle, text.data(), static_cast<DWORD>(text.size()), &bytes, nullptr) && bytes == text.size(),
            "Write private instance file");
    require(FlushFileBuffers(file.handle), "Flush private instance file");
}

inline std::string read_file(const std::wstring &path) {
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
    std::string text;
    char buffer[4096];
    for (;;) {
        DWORD bytes = 0;
        require(ReadFile(file.handle, buffer, sizeof(buffer), &bytes, nullptr), "Read instance log");
        if (!bytes) return text;
        text.append(buffer, bytes);
    }
}
