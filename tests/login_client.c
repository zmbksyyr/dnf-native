/* Captures the launch contract only; it does not implement a game client. */
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR args, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t path[MAX_PATH], cwd[MAX_PATH], address[256], text[4096];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 1;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    wcscpy(slash + 1, L"client-launch.txt");
    GetCurrentDirectoryW(MAX_PATH, cwd);
    GetEnvironmentVariableW(L"GAME_SERVER_IP", address, 256);
    int count = swprintf(text, 4096, L"%ls\n%ls\n%ls\n", args, cwd, address);
    if (count < 0) return 1;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 1;
    DWORD written;
    BOOL ok = WriteFile(file, text, (DWORD)(count * sizeof(wchar_t)), &written, NULL);
    CloseHandle(file);
    return ok && written == count * sizeof(wchar_t) ? 0 : 1;
}
