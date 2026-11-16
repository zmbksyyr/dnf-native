#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <shlobj.h>

#include "crypto.h"
#include "mysql.h"
#include "builtin_key.h"

/* ---------------- constants from the original Go program ---------------- */
static const char UIDT[] =
    "010101010101010101010101010101010101010101010101010101010101010155914510010403030101";

/* ---------------- control ids ---------------- */
#define IDC_DB_IP       1001
#define IDC_DB_PORT     1002
#define IDC_DB_USER     1003
#define IDC_DB_PASS     1004
#define IDC_PLAYER      1005
#define IDC_PLAYER_PW   1006
#define IDC_MSG         1007
#define IDC_BTN_REG     1008
#define IDC_BTN_LOGIN   1009
#define IDC_BTN_KEYS    1010

static HWND g_dbIp, g_dbPort, g_dbUser, g_dbPass, g_player, g_playerPw, g_msg;
static HINSTANCE g_hInst;
static HFONT g_font = NULL;
static RsaKey g_key;
static char *g_key_pem = NULL;
static size_t g_key_pem_len = 0;
static char g_public_pem[1024];
static wchar_t g_exedir[MAX_PATH];
static char g_key_source[512] = "";

/* ---------------- helpers ---------------- */
static void set_msg(const char *text) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *w = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, wlen);
    SetWindowTextW(g_msg, w);
    free(w);
}
static void get_edit_utf8(HWND h, char *out, size_t cap) {
    int wlen = GetWindowTextLengthW(h);
    wchar_t *w = (wchar_t*)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    GetWindowTextW(h, w, wlen + 1);
    int len = WideCharToMultiByte(CP_UTF8, 0, w, wlen, out, (int)cap - 1, NULL, NULL);
    if (len < 0) len = 0;
    out[len] = '\0';
    free(w);
}

/* Build the exe directory and read private_key.pem from it. Returns 1 on
   success (fills pem_out), 0 otherwise. */
static int load_external_key(char **pem_out, size_t *pem_len, char *path_out, size_t path_cap) {
    wchar_t exepath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exepath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(exepath, L'\\');
    if (slash) *(slash + 1) = L'\0'; else exepath[0] = L'\0';
    wcscat_s(exepath, MAX_PATH, L"private_key.pem");

    HANDLE f = CreateFileW(exepath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    DWORD size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > (1024 * 1024)) { CloseHandle(f); return -1; }
    char *buf = (char*)malloc((size_t)size + 1);
    if (!buf) { CloseHandle(f); return -1; }
    DWORD readed = 0;
    if (!ReadFile(f, buf, size, &readed, NULL) || readed != size) { free(buf); CloseHandle(f); return -1; }
    buf[readed] = '\0';
    CloseHandle(f);

    /* convert path to utf8 for display */
    WideCharToMultiByte(CP_UTF8, 0, exepath, -1, path_out, (int)path_cap, NULL, NULL);
    *pem_out = buf;
    *pem_len = readed;
    return 1;
}

static int init_key(void) {
    char path[512] = "";
    int external = load_external_key(&g_key_pem, &g_key_pem_len, path, sizeof(path));
    if (external < 0) return 0;
    if (!external) {
        g_key_pem_len = sizeof(BUILTIN_PRIVATE_KEY) - 1;
        g_key_pem = (char*)malloc(g_key_pem_len + 1);
        if (!g_key_pem) return 0;
        memcpy(g_key_pem, BUILTIN_PRIVATE_KEY, g_key_pem_len + 1);
    }
    /* An invalid external key must not silently select a different key pair. */
    if (rsa_public_key_pem(g_key_pem, g_key_pem_len, g_public_pem, sizeof(g_public_pem)) ||
        rsa_key_from_pem(g_key_pem, g_key_pem_len, &g_key)) return 0;
    snprintf(g_key_source, sizeof(g_key_source), "%s", external ? "外部密钥" : "内置默认密钥");
    return 1;
}

static int write_new_key(const wchar_t *path, const char *data, size_t size) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    int ok = WriteFile(file, data, (DWORD)size, &written, NULL) && written == size;
    if (ok) ok = FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) DeleteFileW(path);
    return ok;
}

static void on_export_keys(HWND owner) {
    BROWSEINFOW browse = {0};
    wchar_t folder[MAX_PATH], private_path[MAX_PATH], public_path[MAX_PATH];
    browse.hwndOwner = owner;
    browse.lpszTitle = L"选择密钥导出目录（不会覆盖已有文件）";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browse);
    if (!selected) return;
    int ok = SHGetPathFromIDListW(selected, folder);
    CoTaskMemFree(selected);
    if (!ok || wcslen(folder) + 20 >= MAX_PATH) {
        set_msg("导出目录无效或路径过长"); return;
    }
    swprintf(private_path, MAX_PATH, L"%ls\\private_key.pem", folder);
    swprintf(public_path, MAX_PATH, L"%ls\\publickey.pem", folder);
    if (GetFileAttributesW(private_path) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(public_path) != INVALID_FILE_ATTRIBUTES) {
        set_msg("目录已有密钥，请选择其他目录"); return;
    }
    if (!write_new_key(private_path, g_key_pem, g_key_pem_len)) {
        set_msg("私钥导出失败，请检查目录权限"); return;
    }
    if (!write_new_key(public_path, g_public_pem, strlen(g_public_pem))) {
        DeleteFileW(private_path);
        set_msg("公钥导出失败，已撤回本次私钥"); return;
    }
    set_msg("密钥对导出成功");
    MessageBoxW(owner,
        L"已导出当前生效的密钥对：\n\n"
        L"private_key.pem：放在客户端的 DNFLogin.exe 旁。\n"
        L"publickey.pem：放在服务端实例 home/neople/game/ 下。\n\n"
        L"替换服务端公钥后需正常重启 game。",
        L"密钥对已导出", MB_OK | MB_ICONINFORMATION);
}

/* Convert UID decimal string -> 8-char zero padded hex, concat UIDT, then
   hex-decode into bytes. Mirrors the original Go logic exactly. */
static int build_payload(const char *uid_dec, uint8_t *out, size_t *out_len) {
    unsigned long long v = strtoull(uid_dec, NULL, 10);
    char hex[32];
    sprintf(hex, "%llx", v);
    size_t hl = strlen(hex);
    char tt[512];
    size_t t = 0;
    int pad = 8 - (int)hl;
    for (int i = 0; i < pad; i++) tt[t++] = '0';
    memcpy(tt + t, hex, hl); t += hl;
    memcpy(tt + t, UIDT, sizeof(UIDT) - 1); t += sizeof(UIDT) - 1;
    tt[t] = '\0';

    size_t dlen = 0;
    for (size_t i = 0; i + 1 < t; i += 2) {
        unsigned int x = 0;
        sscanf(tt + i, "%2x", &x);
        out[dlen++] = (uint8_t)x;
    }
    *out_len = dlen;
    return 1;
}

/* ---------------- business logic ---------------- */
static void on_login(void) {
    /* Resolve the client beside the launcher, independent of shortcut cwd. */
    wchar_t executable[MAX_PATH];
    if (wcslen(g_exedir) + 8 >= MAX_PATH) { set_msg("客户端路径过长"); return; }
    swprintf(executable, MAX_PATH, L"%lsdnf.exe", g_exedir);
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES) {
        set_msg("dnf不存在!");
        return;
    }

    char ip[128], port[32], user[128], dbpass[128], player[128], playerpw[128];
    get_edit_utf8(g_dbIp, ip, sizeof(ip));
    get_edit_utf8(g_dbPort, port, sizeof(port));
    get_edit_utf8(g_dbUser, user, sizeof(user));
    get_edit_utf8(g_dbPass, dbpass, sizeof(dbpass));
    get_edit_utf8(g_player, player, sizeof(player));
    get_edit_utf8(g_playerPw, playerpw, sizeof(playerpw));

    MyConn conn;
    if (my_connect(&conn, ip, (unsigned short)atoi(port), user, dbpass) != 0) {
        set_msg("数据库连接失败!");
        return;
    }

    char pwmd5[33];
    md5_hex((const uint8_t*)playerpw, strlen(playerpw), pwmd5);

    char e_player[512], e_pw[512];
    my_escape_string(player, e_player);
    my_escape_string(pwmd5, e_pw);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "select UID from d_taiwan.accounts where accountname='%s' and password='%s' limit 1",
             e_player, e_pw);

    char *uid = NULL;
    if (my_query_one(&conn, sql, &uid) != 0) {
        set_msg("数据库查询失败!");
        my_close(&conn);
        return;
    }
    if (!uid) {
        set_msg("账号密码错误或不存在!");
        my_close(&conn);
        return;
    }

    uint8_t payload[256];
    size_t plen = 0;
    build_payload(uid, payload, &plen);
    free(uid);
    my_close(&conn);

    uint8_t enc[512];
    int elen = rsa_private_encrypt(&g_key, payload, plen, enc);
    if (elen < 0) { set_msg("加密失败!"); return; }
    char b64[1024];
    base64_encode(enc, (size_t)elen, b64);

    /* A preconfigured client game address wins; otherwise use the database
       address for the original launcher's single-host setup. No server file IO. */
    char configured_ip[128];
    int inherited_ip = GetEnvironmentVariableA("GAME_SERVER_IP", configured_ip, sizeof(configured_ip)) != 0;
    if (!inherited_ip && !SetEnvironmentVariableA("GAME_SERVER_IP", ip)) {
        set_msg("无法设置客户端游戏地址"); return;
    }
    wchar_t command[1600];
    swprintf(command, 1600, L"\"%ls\" %hs", executable, b64);
    STARTUPINFOW si = {0}; PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    BOOL started = CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, g_exedir, &si, &pi);
    if (!inherited_ip) SetEnvironmentVariableA("GAME_SERVER_IP", NULL);
    if (started) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        set_msg("登录成功!");
    } else {
        set_msg("启动dnf.exe失败!");
    }
}

static void on_register(void) {
    char ip[128], port[32], user[128], dbpass[128], player[128], playerpw[128];
    get_edit_utf8(g_dbIp, ip, sizeof(ip));
    get_edit_utf8(g_dbPort, port, sizeof(port));
    get_edit_utf8(g_dbUser, user, sizeof(user));
    get_edit_utf8(g_dbPass, dbpass, sizeof(dbpass));
    get_edit_utf8(g_player, player, sizeof(player));
    get_edit_utf8(g_playerPw, playerpw, sizeof(playerpw));

    MyConn conn;
    if (my_connect(&conn, ip, (unsigned short)atoi(port), user, dbpass) != 0) {
        set_msg("数据库连接失败!");
        return;
    }

    char e_player[512];
    my_escape_string(player, e_player);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "select UID from d_taiwan.accounts where accountname='%s' limit 1", e_player);
    char *existing = NULL;
    if (my_query_one(&conn, sql, &existing) != 0) { set_msg("数据库查询失败!"); my_close(&conn); return; }
    if (existing) {
        free(existing);
        set_msg("账号已存在！");
        my_close(&conn);
        return;
    }

    /* find next UID */
    char newuid[32];
    char *maxuid = NULL;
    if (my_query_one(&conn, "select UID from d_taiwan.accounts order by UID desc limit 1", &maxuid) != 0) {
        set_msg("数据库查询失败!"); my_close(&conn); return;
    }
    if (maxuid) {
        long long u = strtoll(maxuid, NULL, 10);
        snprintf(newuid, sizeof(newuid), "%lld", u + 1);
        free(maxuid);
    } else {
        strcpy(newuid, "10001");
    }

    char pwmd5[33];
    md5_hex((const uint8_t*)playerpw, strlen(playerpw), pwmd5);
    char e_pw[512];
    my_escape_string(pwmd5, e_pw);
    /* newuid is numeric, safe to inline */

    char ins[1024];
    snprintf(ins, sizeof(ins),
             "insert into d_taiwan.accounts(`UID`, `accountname`, `password`) VALUES(%s,'%s','%s')",
             newuid, e_player, e_pw);
    if (my_exec(&conn, ins) != 0) { set_msg("账号创建失败！"); my_close(&conn); return; }

    snprintf(ins, sizeof(ins), "insert into d_taiwan.member_info (m_id,user_id) VALUES (%s,%s)", newuid, newuid);
    if (my_exec(&conn, ins) != 0) { set_msg("账号资料写入失败，请联系管理员检查四表"); my_close(&conn); return; }
    snprintf(ins, sizeof(ins), "insert into d_taiwan.member_white_account (m_id) VALUES (%s)", newuid);
    if (my_exec(&conn, ins) != 0) { set_msg("账号资料写入失败，请联系管理员检查四表"); my_close(&conn); return; }
    snprintf(ins, sizeof(ins), "insert into taiwan_login.member_login (m_id) VALUES (%s)", newuid);
    if (my_exec(&conn, ins) != 0) { set_msg("账号资料写入失败，请联系管理员检查四表"); my_close(&conn); return; }

    my_close(&conn);
    set_msg("账号创建成功！");
}

/* ---------------- GUI ---------------- */
static HWND make_edit(HWND parent, int id, int x, int y, int w, int h,
                      const wchar_t *text) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP;
    HWND e = CreateWindowExW(0, L"EDIT", text, style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, g_hInst, NULL);
    return e;
}
static void make_label(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h,
                    parent, NULL, g_hInst, NULL);
}
static void make_button(HWND parent, const wchar_t *text, int id, int x, int y, int w, int h) {
    CreateWindowExW(0, L"BUTTON", text,
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                    x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
}
static void make_group(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    CreateWindowExW(0, L"BUTTON", text,
                    WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                    x, y, w, h, parent, NULL, g_hInst, NULL);
}
static HFONT make_font(int height_pt, int bold) {
    return CreateFontW(-height_pt * 96 / 72, 0, 0, 0,
                       bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static BOOL CALLBACK set_font_child(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            enum { LEFT = 14, GROUP_W = 288, LBL_X = 26, LBL_W = 76, ED_X = 108, ED_W = 180 };
            g_font = make_font(9, 0);

            /* ---- key source: framed box at the top ---- */
            make_group(hwnd, L"密钥", LEFT, 12, GROUP_W, 70);
            {
                int wl = MultiByteToWideChar(CP_UTF8, 0, g_key_source, -1, NULL, 0);
                wchar_t *w = (wchar_t*)malloc((size_t)wl * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, g_key_source, -1, w, wl);
                CreateWindowExW(0, L"STATIC", w, WS_CHILD | WS_VISIBLE | SS_CENTER,
                                LEFT + 8, 38, 128, 22, hwnd, NULL, g_hInst, NULL);
                free(w);
            }

            make_button(hwnd, L"导出密钥对", IDC_BTN_KEYS, 156, 34, 132, 28);

            /* ---- group 1: database connection ---- */
            make_group(hwnd, L"数据库连接", LEFT, 90, GROUP_W, 136);
            make_label(hwnd, L"数据库地址:", LBL_X, 113, LBL_W, 18);
            g_dbIp   = make_edit(hwnd, IDC_DB_IP,   ED_X, 110,  ED_W, 22, L"127.0.0.1");
            make_label(hwnd, L"端口:", LBL_X, 141, LBL_W, 18);
            g_dbPort = make_edit(hwnd, IDC_DB_PORT, ED_X, 138, ED_W, 22, L"3306");
            make_label(hwnd, L"MySQL账号:", LBL_X, 169, LBL_W, 18);
            g_dbUser = make_edit(hwnd, IDC_DB_USER, ED_X, 166, ED_W, 22, L"game");
            make_label(hwnd, L"MySQL密码:", LBL_X, 197, LBL_W, 18);
            g_dbPass = make_edit(hwnd, IDC_DB_PASS, ED_X, 194, ED_W, 22, L"");

            /* ---- group 2: game account ---- */
            make_group(hwnd, L"游戏账号", LEFT, 234, GROUP_W, 82);
            make_label(hwnd, L"游戏账号:", LBL_X, 257, LBL_W, 18);
            g_player   = make_edit(hwnd, IDC_PLAYER,    ED_X, 254, ED_W, 22, L"");
            make_label(hwnd, L"游戏密码:", LBL_X, 285, LBL_W, 18);
            g_playerPw = make_edit(hwnd, IDC_PLAYER_PW, ED_X, 282, ED_W, 22, L"");

            g_msg = CreateWindowExW(0, L"STATIC", L"",
                                    WS_CHILD | WS_VISIBLE | SS_CENTER, LEFT, 324, GROUP_W, 22,
                                    hwnd, (HMENU)(INT_PTR)IDC_MSG, g_hInst, NULL);

            make_button(hwnd, L"注 册", IDC_BTN_REG,   30,  352, 120, 30);
            make_button(hwnd, L"登 录", IDC_BTN_LOGIN, 166, 352, 120, 30);

            SendMessageW(g_dbPass, EM_SETPASSWORDCHAR, L'●', 0);
            SendMessageW(g_playerPw, EM_SETPASSWORDCHAR, L'●', 0);
            EnumChildWindows(hwnd, set_font_child, (LPARAM)g_font);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_LOGIN) on_login();
            else if (id == IDC_BTN_REG) on_register();
            else if (id == IDC_BTN_KEYS) on_export_keys(hwnd);
            return 0;
        }
        case WM_DESTROY:
            if (g_font) DeleteObject(g_font);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrev; (void)lpCmdLine;
    g_hInst = hInst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DWORD path_len = GetModuleFileNameW(NULL, g_exedir, MAX_PATH);
    if (!path_len || path_len >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(g_exedir, L'\\');
    if (!slash) return 1;
    slash[1] = 0;
    if (!init_key()) {
        MessageBoxW(NULL, L"密钥加载失败。请检查登录器旁的 private_key.pem（仅支持有效 PKCS#1 RSA 私钥）。", L"错误", MB_ICONERROR);
        return 1;
    }

    const wchar_t *cls = L"DNFLoginWndClass";
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc)) { MessageBoxW(NULL, L"窗口注册失败", L"错误", MB_ICONERROR); return 1; }

    HWND hwnd = CreateWindowExW(0, cls, L"DNF 简单登录器",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 332, 432,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) { MessageBoxW(NULL, L"窗口创建失败", L"错误", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    free(g_key_pem);
    CoUninitialize();
    return (int)m.wParam;
}
