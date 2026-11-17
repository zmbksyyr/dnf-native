// Exercise the production sampler against a live tree and an old-parent PID
// alias. The injected ancestry is confined to this test's snapshot view.
#include <winsock2.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>

static DWORD inspected_pid;
static bool stale_parent;
static unsigned injected;
static BOOL snapshot_entry(BOOL found,LPPROCESSENTRY32W entry) {
    if(found && stale_parent && !_wcsicmp(entry->szExeFile,L"csrss.exe")) {
        entry->th32ParentProcessID=inspected_pid;
        ++injected;
    }
    return found;
}
static BOOL first_entry(HANDLE snapshot,LPPROCESSENTRY32W entry) {
    return snapshot_entry(Process32FirstW(snapshot,entry),entry);
}
static BOOL next_entry(HANDLE snapshot,LPPROCESSENTRY32W entry) {
    return snapshot_entry(Process32NextW(snapshot,entry),entry);
}

#define GetCurrentProcessId() inspected_pid
#define Process32FirstW first_entry
#define Process32NextW next_entry
#include "../src/controller/controller_gui.cpp"
#undef Process32NextW
#undef Process32FirstW
#undef GetCurrentProcessId

int wmain(int argc,wchar_t **argv) {
    if(argc!=3) return 2;
    inspected_pid=wcstoul(argv[1],nullptr,10);
    stale_parent=wcstoul(argv[2],nullptr,10)!=0;
    App state;
    state.window=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!state.window) return 3;
    int result=0;
    for(unsigned i=0;i<4;++i) {
        state.update_title_metrics();
        auto text=window_text(state.window);
        char utf8[640];
        int bytes=WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),utf8,sizeof(utf8),nullptr,nullptr);
        if(bytes>0) std::fwrite(utf8,1,static_cast<size_t>(bytes),stdout);
        std::putchar('\n');
        if(text.find(L"--")!=std::wstring::npos || text.find(L"MB")==std::wstring::npos) result=4;
        Sleep(550);
    }
    if(stale_parent && !injected) result=5;
    std::printf("stale-parent-entries=%u\n",injected);
    DestroyWindow(state.window);
    return result;
}
