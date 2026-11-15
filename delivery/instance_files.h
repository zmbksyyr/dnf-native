// An instance lease prevents a second controller from deleting a live instance.
#pragma once
static HANDLE lease_instance(const std::wstring &path) {
    HANDLE handle = CreateFileW((path + L"\\.instance.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Instance is already open in another controller");
    return handle;
}

static bool known_instance(const std::wstring &path) {
    return present(path + L"\\services.ini") || present(path + L"\\instance.pending");
}

static void remove_instance_tree(const std::wstring &path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES, "Read instance file attributes");
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW item;
        HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &item);
        if (search != INVALID_HANDLE_VALUE) {
            try {
                do {
                    if (std::wcscmp(item.cFileName, L".") && std::wcscmp(item.cFileName, L".."))
                        remove_instance_tree(path + L"\\" + item.cFileName);
                } while (FindNextFileW(search, &item));
            } catch (...) { FindClose(search); throw; }
            FindClose(search);
        }
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        require(SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY), "Clear instance file read-only flag");
    require(attributes & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str()), "Delete instance file or directory");
}
