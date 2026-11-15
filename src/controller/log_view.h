// Read the original service files without changing application buffering or log contents.
#pragma once
#include "../win32_files.h"
#include <algorithm>

struct LogSource { std::wstring label, path; ULONGLONG modified = 0; bool preferred = false; };

static ULONGLONG log_time(const FILETIME &time) {
    ULARGE_INTEGER value; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

static void application_logs(const std::wstring &folder, const std::wstring &relative, std::vector<LogSource> &logs) {
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = entry.cFileName;
        if (name == L"." || name == L".." || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::wstring path = folder + L"\\" + name, label = relative + name;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) application_logs(path, label + L"/", logs);
        else {
            bool preferred = name.size() >= 5 && name.substr(name.size() - 5) == L".init";
            preferred = preferred || name.rfind(L"Info_", 0) == 0 || name.rfind(L"process", 0) == 0;
            logs.push_back({L"应用日志 · " + label, path, log_time(entry.ftLastWriteTime), preferred});
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

static std::wstring latest_log(const std::wstring &folder, const std::wstring &pattern) {
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW((folder + L"\\" + pattern).c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return {};
    std::wstring result; ULONGLONG newest = 0;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULONGLONG time = log_time(entry.ftLastWriteTime);
        if (result.empty() || time > newest) { newest = time; result = folder + L"\\" + entry.cFileName; }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return result;
}

struct LogTail { std::string bytes; ULONGLONG size = 0, modified = 0; bool truncated = false; };

static LogTail read_log_tail(const std::wstring &path) {
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size;
    FILETIME modified;
    require(GetFileSizeEx(file.handle, &size) && GetFileTime(file.handle, nullptr, nullptr, &modified), "Read log metadata");
    LogTail result; result.size = size.QuadPart; result.modified = log_time(modified);
    constexpr LONGLONG limit = 256 * 1024;
    LARGE_INTEGER offset; offset.QuadPart = size.QuadPart > limit ? size.QuadPart - limit : 0;
    result.truncated = offset.QuadPart != 0;
    require(SetFilePointerEx(file.handle, offset, nullptr, FILE_BEGIN), "Seek log tail");
    result.bytes.resize(static_cast<size_t>(size.QuadPart - offset.QuadPart));
    DWORD read = 0;
    require(ReadFile(file.handle, result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &read, nullptr), "Read original log");
    result.bytes.resize(read);
    if (result.truncated) {
        size_t newline = result.bytes.find('\n');
        if (newline != std::string::npos) result.bytes.erase(0, newline + 1);
    }
    return result;
}

static std::wstring log_text(const std::string &bytes, unsigned encoding) {
    if (bytes.empty()) return {};
    const UINT pages[] = {CP_UTF8, CP_UTF8, 936, 950, 949};
    UINT page = pages[encoding < 5 ? encoding : 0];
    int count = MultiByteToWideChar(page, encoding ? 0 : MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!count && !encoding) { page = CP_ACP; count = MultiByteToWideChar(page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0); }
    std::wstring text(count, L'\0');
    MultiByteToWideChar(page, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), count);
    std::wstring lines;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (!i || text[i - 1] != L'\r')) lines += L'\r';
        lines += text[i];
    }
    return lines;
}
