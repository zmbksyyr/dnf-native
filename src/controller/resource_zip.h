// ZIP extraction through Win32 handles keeps Unicode paths and avoids external tools.
#pragma once
#include "../win32_files.h"
#include "../third_party/miniz/miniz.h"
#include <bcrypt.h>
#include <map>
#include <set>

static std::string file_sha256(const std::wstring &path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        throw std::runtime_error("Open SHA256 provider failed");
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) {
        BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("Create SHA256 failed");
    }
    try {
        File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        BYTE buffer[65536], digest[32]; DWORD count;
        for (;;) {
            require(ReadFile(file.handle, buffer, sizeof(buffer), &count, nullptr), "Read resource");
            if (!count) break;
            if (BCryptHashData(hash, buffer, count, 0)) throw std::runtime_error("Hash resource failed");
        }
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0)) throw std::runtime_error("Finish SHA256 failed");
        std::string text;
        for (BYTE value : digest) { text += "0123456789abcdef"[value >> 4]; text += "0123456789abcdef"[value & 15]; }
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); return text;
    } catch (...) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
}

static std::wstring zip_path(const std::string &name) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid ZIP filename encoding");
    std::wstring path(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), path.data(), size);
    if (path.front() == L'/' || path.find_first_of(L"\\:") != std::wstring::npos)
        throw std::runtime_error("ZIP path must be relative");
    size_t at = 0;
    while (at < path.size()) {
        size_t end = path.find(L'/', at);
        std::wstring part = path.substr(at, end == std::wstring::npos ? end : end - at);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
            throw std::runtime_error("Invalid ZIP path component");
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

class ResourceZip {
    File file;
    mz_zip_archive zip = {};
    static size_t read_at(void *context, mz_uint64 offset, void *buffer, size_t bytes) {
        HANDLE handle = static_cast<ResourceZip *>(context)->file.handle;
        LARGE_INTEGER at; at.QuadPart = static_cast<LONGLONG>(offset); DWORD read = 0;
        if (bytes > MAXDWORD || !SetFilePointerEx(handle, at, nullptr, FILE_BEGIN) ||
            !ReadFile(handle, buffer, static_cast<DWORD>(bytes), &read, nullptr)) return 0;
        return read;
    }
    static size_t write_at(void *context, mz_uint64, const void *buffer, size_t bytes) {
        DWORD written = 0;
        if (bytes > MAXDWORD || !WriteFile(static_cast<File *>(context)->handle, buffer, static_cast<DWORD>(bytes), &written, nullptr)) return 0;
        return written;
    }
public:
    explicit ResourceZip(const std::wstring &path) : file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                                                     nullptr, OPEN_EXISTING, 0, nullptr)) {
        LARGE_INTEGER size; require(GetFileSizeEx(file.handle, &size), "Read ZIP size");
        zip.m_pRead = read_at; zip.m_pIO_opaque = this;
        if (!mz_zip_reader_init(&zip, size.QuadPart, 0)) throw std::runtime_error("Open resource ZIP failed");
    }
    ~ResourceZip() { mz_zip_reader_end(&zip); }
    std::map<std::string, unsigned> files() {
        std::map<std::string, unsigned> result;
        for (unsigned n = 0; n < mz_zip_reader_get_num_files(&zip); ++n) {
            mz_zip_archive_file_stat info;
            if (!mz_zip_reader_file_stat(&zip, n, &info)) throw std::runtime_error("Read ZIP entry failed");
            std::string name = info.m_filename;
            if (mz_zip_reader_get_filename(&zip, n, nullptr, 0) != name.size() + 1)
                throw std::runtime_error("ZIP entry name is too long");
            zip_path(name);
            if (info.m_is_directory) continue;
            if (info.m_is_encrypted || !info.m_is_supported || ((info.m_external_attr >> 16) & 0170000) == 0120000)
                throw std::runtime_error("Unsupported ZIP entry");
            if (!result.emplace(name, n).second) throw std::runtime_error("Duplicate ZIP entry");
        }
        return result;
    }
    void extract(const std::map<std::string, std::wstring> &targets) {
        auto entries = files();
        if (entries.size() != targets.size()) throw std::runtime_error("ZIP file list differs from manifest");
        for (const auto &[name, destination] : targets) {
            auto item = entries.find(name);
            if (item == entries.end()) throw std::runtime_error("ZIP resource is missing: " + name);
            directory(destination.substr(0, destination.find_last_of(L'\\')));
            File output = private_file(destination);
            if (!mz_zip_reader_extract_to_callback(&zip, item->second, write_at, &output, 0))
                throw std::runtime_error("Extract ZIP resource failed: " + name);
            require(FlushFileBuffers(output.handle), "Flush ZIP resource");
        }
    }
};
