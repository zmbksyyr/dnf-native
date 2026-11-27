// Presence checks are cheap enough to refresh when the user changes resources.
#pragma once
static const wchar_t *package_files[] = {
    L"mysql.zip", L"dnf-host-x86.exe", L"dnf-host-x64.exe", L"native.zip", L"mysql-control.exe", L"robot.exe"
};

static std::wstring missing_resources(const std::wstring &folder) {
    DWORD manifest_attributes = GetFileAttributesW((folder + L"\\resources.ini").c_str());
    std::wstring missing = manifest_attributes == INVALID_FILE_ATTRIBUTES || (manifest_attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? L"resources.ini" : L"";
    for (const wchar_t *name : package_files) {
        DWORD attributes = GetFileAttributesW((folder + L"\\" + name).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!missing.empty()) missing += L"、";
            missing += name;
        }
    }
    return missing;
}

static void verify_package(const std::wstring &folder) {
    if (!missing_resources(folder).empty()) throw std::runtime_error("Required package resources are missing");
    std::wstring manifest = folder + L"\\resources.ini";
    if (setting(manifest, L"resources", L"version") != L"4")
        throw std::runtime_error("Unsupported resource manifest version");
    for (const wchar_t *name : package_files) {
        std::wstring expected = setting(manifest, L"package", name);
        if (expected.size() != 64 || file_sha256(folder + L"\\" + name) != std::string(expected.begin(), expected.end()))
            throw std::runtime_error("Package resource hash differs: " + std::string(name, name + std::wcslen(name)));
    }
}
