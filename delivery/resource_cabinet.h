// Native extraction of this project's content-addressed local resource CAB.
#pragma once
#include <fdi.h>
#include <cstdlib>
#include <map>

static thread_local std::wstring resource_cabinet_source;

static void *DIAMONDAPI resource_alloc(ULONG bytes) { return std::malloc(bytes); }
static void DIAMONDAPI resource_free(void *memory) { std::free(memory); }
static INT_PTR DIAMONDAPI resource_open(char *name, int, int) {
    if (std::strcmp(name, "resources.cab")) return -1;
    return reinterpret_cast<INT_PTR>(CreateFileW(resource_cabinet_source.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}
static UINT DIAMONDAPI resource_read(INT_PTR file, void *buffer, UINT bytes) {
    DWORD count = 0;
    return ReadFile(reinterpret_cast<HANDLE>(file), buffer, bytes, &count, nullptr) ? count : UINT(-1);
}
static UINT DIAMONDAPI resource_write(INT_PTR file, void *buffer, UINT bytes) {
    DWORD count = 0;
    return WriteFile(reinterpret_cast<HANDLE>(file), buffer, bytes, &count, nullptr) ? count : UINT(-1);
}
static int DIAMONDAPI resource_close(INT_PTR file) { return CloseHandle(reinterpret_cast<HANDLE>(file)) ? 0 : -1; }
static long DIAMONDAPI resource_seek(INT_PTR file, long offset, int origin) {
    DWORD result = SetFilePointer(reinterpret_cast<HANDLE>(file), offset, nullptr, origin);
    return result == INVALID_SET_FILE_POINTER ? -1 : static_cast<long>(result);
}

struct ResourceCabinet {
    const std::map<std::string, std::wstring> &targets;
    HANDLE output = INVALID_HANDLE_VALUE;
    size_t count = 0;
    std::string error;
    ~ResourceCabinet() { if (output != INVALID_HANDLE_VALUE) CloseHandle(output); }
};

static INT_PTR DIAMONDAPI resource_notify(FDINOTIFICATIONTYPE kind, PFDINOTIFICATION info) {
    auto &context = *static_cast<ResourceCabinet *>(info->pv);
    try {
        if (kind == fdintCOPY_FILE) {
            auto found = context.targets.find(info->psz1);
            if (found == context.targets.end()) throw std::runtime_error("Resource cabinet contains an unlisted object");
            const std::wstring &path = found->second;
            directory(path.substr(0, path.find_last_of(L'\\')));
            File output = private_file(path);
            context.output = output.handle; output.handle = INVALID_HANDLE_VALUE;
            return reinterpret_cast<INT_PTR>(context.output);
        }
        if (kind == fdintCLOSE_FILE_INFO) {
            require(FlushFileBuffers(context.output), "Flush extracted game resource");
            require(CloseHandle(context.output), "Close extracted game resource");
            context.output = INVALID_HANDLE_VALUE; ++context.count; return TRUE;
        }
        if (kind == fdintNEXT_CABINET || kind == fdintPARTIAL_FILE)
            throw std::runtime_error("Local resources require one complete cabinet");
        return 0;
    } catch (const std::exception &error) { context.error = error.what(); return -1; }
}

static void extract_resource_cabinet(const std::wstring &source, const std::map<std::string, std::wstring> &targets) {
    resource_cabinet_source = source;
    ERF error = {};
    HFDI reader = FDICreate(resource_alloc, resource_free, resource_open, resource_read,
        resource_write, resource_close, resource_seek, cpuUNKNOWN, &error);
    if (!reader) throw std::runtime_error("Create native resource cabinet reader failed");
    ResourceCabinet context{targets, INVALID_HANDLE_VALUE, 0, {}};
    char name[] = "resources.cab", folder[] = "";
    bool copied = FDICopy(reader, name, folder, 0, resource_notify, nullptr, &context);
    FDIDestroy(reader); resource_cabinet_source.clear();
    if (!copied || context.count != targets.size())
        throw std::runtime_error(context.error.empty() ? "Extract local game resources failed" : context.error);
}
