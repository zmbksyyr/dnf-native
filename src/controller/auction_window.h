// One maintenance window; all operations use the controller's single worker.
#pragma once
static void market_summary() {
    if(!app->market_window) return;
    std::wstring s=L"当前 PVF：";
    try { s+=market::game_pvf(app->services); } catch(...) { s+=L"未配置"; }
    s+=L"\r\n";
    for(const auto &v:app->services) if(v.name==L"auction" || v.name==L"point") s+=v.name+L"："+(running(v)?L"进程运行中（业务查看日志）":L"已停止")+L"   ";
    SetWindowTextW(GetDlgItem(app->market_window,MARKET_SUMMARY),s.c_str());
}
static LRESULT CALLBACK market_proc(HWND w,UINT message,WPARAM wp,LPARAM lp) {
    try {
        switch(message) {
        case WM_CREATE: {
            app->market_window=w;
            auto add=[&](const wchar_t *type,const wchar_t *text,DWORD style,int id,int x,int y,int width,int height) {
                HWND c=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,app->px(x),app->px(y),app->px(width),app->px(height),w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
                require(c!=nullptr,"Create market maintenance control"); SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(app->font),TRUE);
                if(!wcscmp(type,L"BUTTON") && (style&BS_TYPEMASK)==BS_OWNERDRAW) SetWindowSubclass(c,button_proc,1,0);
                if(!wcscmp(type,L"BUTTON") && (style&BS_TYPEMASK)==BS_AUTOCHECKBOX)
                    SetWindowLongPtrW(c,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(switch_proc));
                return c;
            };
            add(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL,MARKET_SUMMARY,24,20,812,65);
            add(L"STATIC",L"兼容设置",0,320,24,101,800,23);
            add(L"BUTTON",L"扩展等级检索 · 匹配 PVF 装备等级",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_LEVELS,24,133,390,30);
            add(L"BUTTON",L"锻造兼容 · 同时处理搜索和均价",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_REFINE,434,133,402,30);
            add(L"BUTTON",L"DP2 搜索保护 · 仅有效孔／宝石记录覆盖",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_DP2,24,170,400,30);
            add(L"BUTTON",L"PVF 变化时自动检查索引与存量订单",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_AUTO,434,170,402,30);
            auto o=market::options(app->current);
            for(auto p:{std::pair<int,bool>{MARKET_LEVELS,o.levels},{MARKET_REFINE,o.refine},{MARKET_DP2,o.dp2},{MARKET_AUTO,o.automatic}}) SendMessageW(GetDlgItem(w,p.first),BM_SETCHECK,p.second?BST_CHECKED:BST_UNCHECKED,0);
            bool frida=setting(app->current+L"\\services.ini",L"plugins",L"mode")==L"frida";
            bool compatible=true;
            try { market::dp2_script(app->resources,app->current,true); }
            catch(const std::exception &e) {compatible=false;app->market_report=L"DP2 搜索保护不可用："+decode(e.what());}
            // Exact script compatibility is also checked before writing.
            EnableWindow(GetDlgItem(w,MARKET_DP2),(frida && compatible) || o.dp2);
            add(L"STATIC",L"索引维护 · 自动备份；存在冲突时保留订单并说明原因；双月表由程序自动维护。",0,321,24,218,812,25);
            add(L"BUTTON",L"检查兼容性",BS_OWNERDRAW|WS_TABSTOP,market::Check,24,252,176,34);
            add(L"BUTTON",L"重建并同步 iteminfo",BS_OWNERDRAW|WS_TABSTOP,market::Sync,212,252,210,34);
            add(L"BUTTON",L"恢复上一版索引",BS_OWNERDRAW|WS_TABSTOP,market::RestoreIndex,434,252,182,34);
            HWND report=add(L"EDIT",app->market_report.c_str(),ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,MARKET_REPORT,24,304,812,185);
            SendMessageW(report,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(app->px(8),app->px(8)));
            add(L"STATIC",L"数据维护",SS_CENTERIMAGE,322,24,507,100,30);
            HWND scope=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,MARKET_SCOPE,136,507,300,160);
            for(auto label:{L"金币拍卖",L"点券拍卖（金币寄售）",L"两者"}) SendMessageW(scope,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            SendMessageW(scope,CB_SETCURSEL,2,0);
            add(L"STATIC",L"红字动作直接删除所选数据，不返还物品或货币；先自动备份并停相关服务。",SS_CENTERIMAGE,323,24,546,812,25);
            add(L"BUTTON",L"清理不兼容订单",BS_OWNERDRAW|WS_TABSTOP,market::CleanInvalid,24,582,192,36);
            add(L"BUTTON",L"清空在售订单",BS_OWNERDRAW|WS_TABSTOP,market::ClearOrders,228,582,192,36);
            add(L"BUTTON",L"清空均价数据",BS_OWNERDRAW|WS_TABSTOP,market::ClearPrices,432,582,192,36);
            add(L"BUTTON",L"重置拍卖库",BS_OWNERDRAW|WS_TABSTOP,market::Reset,636,582,200,36);
            add(L"BUTTON",L"应用并重启相关服务",BS_OWNERDRAW|WS_TABSTOP,market::Apply,568,644,268,38);
            add(L"BUTTON",L"关闭",BS_OWNERDRAW|WS_TABSTOP,MARKET_CLOSE,24,644,100,38);
            market_summary(); return 0;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wp),RGB(255,255,255)); SetTextColor(reinterpret_cast<HDC>(wp),RGB(54,77,112)); return reinterpret_cast<LRESULT>(app->surface);
        case WM_ERASEBKGND: { RECT r;GetClientRect(w,&r);FillRect(reinterpret_cast<HDC>(wp),&r,app->surface);return 1; }
        case WM_DRAWITEM: draw_item(*reinterpret_cast<DRAWITEMSTRUCT *>(lp)); return TRUE;
        case WM_COMMAND: {
            int id=LOWORD(wp);
            if(id==MARKET_CLOSE) { SendMessageW(w,WM_CLOSE,0,0); return 0; }
            if(id>=market::Check && id<=market::Reset && HIWORD(wp)==BN_CLICKED && !app->busy) {
                int scope=(int)SendMessageW(GetDlgItem(w,MARKET_SCOPE),CB_GETCURSEL,0,0);
                if(id<=market::Apply) scope=2;
                app->action(id,L"",scope); return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            if(app->busy) return 0;
            DestroyWindow(w); return 0;
        case WM_DESTROY:
            app->market_window=nullptr; EnableWindow(app->window,TRUE); SetActiveWindow(app->window); app->controls(); return 0;
        }
    } catch(const std::exception &e) { SetWindowTextW(GetDlgItem(w,MARKET_REPORT),decode(e.what()).c_str()); }
    return DefWindowProcW(w,message,wp,lp);
}
static void open_market_window() {
    if(app->market_window) { SetForegroundWindow(app->market_window); return; }
    WNDCLASSW cls{}; cls.lpfnWndProc=market_proc; cls.hInstance=GetModuleHandleW(nullptr); cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName=L"DnfNativeMarketMaintenance";
    if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) require(false,"Register market maintenance window");
    RECT size{0,0,app->px(860),app->px(706)}; AdjustWindowRectEx(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_CONTROLPARENT);
    RECT owner;GetWindowRect(app->window,&owner);
    HWND w=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"双拍卖维护",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        std::max(0L,owner.left+(owner.right-owner.left-(size.right-size.left))/2),std::max(0L,owner.top+16),size.right-size.left,size.bottom-size.top,app->window,nullptr,cls.hInstance,nullptr);
    require(w!=nullptr,"Open market maintenance window"); EnableWindow(app->window,FALSE); ShowWindow(w,SW_SHOW);
}
