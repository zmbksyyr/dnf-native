// One maintenance window; all operations use the controller's single worker.
#pragma once
static void market_summary() {
    if(!app->market_window) return;
    app->market_pvf_text=L"当前 PVF：";
    try { app->market_pvf_text+=market::game_pvf(app->services); } catch(...) { app->market_pvf_text+=L"未配置"; }
    SetWindowTextW(GetDlgItem(app->market_window,MARKET_SUMMARY),app->market_pvf_text.c_str());
    std::wstring state;
    for(const auto &v:app->services) if(v.name==L"auction" || v.name==L"point") state+=v.name+L"："+(running(v)?L"运行中":L"已停止")+L"   ";
    SetWindowTextW(GetDlgItem(app->market_window,MARKET_STATUS),state.c_str());
    if(app->market_tooltip) {
        TOOLINFOW info{}; info.cbSize=sizeof(info); info.hwnd=app->market_window;
        info.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(app->market_window,MARKET_SUMMARY));
        info.lpszText=app->market_pvf_text.data();
        SendMessageW(app->market_tooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&info));
    }
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
                if(!wcscmp(type,L"COMBOBOX")) {
                    SetWindowTheme(c,L"",L""); SetWindowSubclass(c,combo_proc,1,0);
                    SendMessageW(c,CB_SETITEMHEIGHT,0,app->px(30));
                }
                return c;
            };
            if(!app->banner) app->banner=CreateSolidBrush(RGB(247,250,255));
            add(L"STATIC",L"",SS_PATHELLIPSIS|SS_NOPREFIX|SS_CENTERIMAGE,MARKET_SUMMARY,28,18,816,18);
            add(L"STATIC",L"",SS_CENTERIMAGE,MARKET_STATUS,28,38,816,18);
            add(L"STATIC",L"运行设置",SS_CENTERIMAGE,325,32,78,96,22);
            add(L"STATIC",L"等级上限",SS_CENTERIMAGE,LEVEL_LABEL,32,108,76,30);
            HWND level=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_TABSTOP,LEVEL_CAP,116,108,90,30);
            for(unsigned value:game_level_choices) {
                std::wstring label=std::to_wstring(value)+L" 级";
                SendMessageW(level,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(level,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),app->px(28));
            add(L"BUTTON",L"DP2",BS_AUTOCHECKBOX|WS_TABSTOP,DP2_ENABLE,264,108,80,30);
            add(L"BUTTON",L"Frida",BS_AUTOCHECKBOX|WS_TABSTOP,FRIDA_ENABLE,452,108,84,30);
            add(L"BUTTON",L"应用并重启",BS_OWNERDRAW|WS_TABSTOP,LEVEL_APPLY,674,108,174,30);
            add(L"STATIC",L"兼容设置",SS_CENTERIMAGE,322,32,170,96,22);
            add(L"BUTTON",L"扩展等级检索 · 匹配 PVF 装备等级",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_LEVELS,32,198,396,30);
            add(L"BUTTON",L"锻造兼容 · 同时处理搜索和均价",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_REFINE,452,198,396,30);
            add(L"BUTTON",L"DP2 搜索保护 · 仅有效孔／宝石记录覆盖",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_DP2,32,234,396,30);
            add(L"BUTTON",L"PVF 变化时自动检查索引与存量订单",BS_AUTOCHECKBOX|WS_TABSTOP,MARKET_AUTO,452,234,396,30);
            auto o=market::options(app->current);
            for(auto p:{std::pair<int,bool>{MARKET_LEVELS,o.levels},{MARKET_REFINE,o.refine},{MARKET_DP2,o.dp2},{MARKET_AUTO,o.automatic}}) SendMessageW(GetDlgItem(w,p.first),BM_SETCHECK,p.second?BST_CHECKED:BST_UNCHECKED,0);
            app->market_guard_supported=true;
            try { market::dp2_script(app->resources,app->current,true); }
            catch(const std::exception &e) {app->market_guard_supported=false;app->notice(L"DP2 搜索保护不可用："+decode(e.what()));}
            // Exact script compatibility is also checked before writing.
            // One maintenance panel: four index actions, then four data actions.
            add(L"STATIC",L"索引维护",SS_CENTERIMAGE,MARKET_INDEX_LABEL,32,288,96,22);
            add(L"BUTTON",L"检查兼容性",BS_OWNERDRAW|WS_TABSTOP,market::Check,32,318,195,38);
            add(L"BUTTON",L"重建并同步 iteminfo",BS_OWNERDRAW|WS_TABSTOP,market::Sync,239,318,195,38);
            add(L"BUTTON",L"恢复上一版索引",BS_OWNERDRAW|WS_TABSTOP,market::RestoreIndex,446,318,195,38);
            add(L"BUTTON",L"应用并重启相关服务",BS_OWNERDRAW|WS_TABSTOP,market::Apply,653,318,195,38);
            add(L"STATIC",L"数据维护",SS_CENTERIMAGE,MARKET_DATA_LABEL,32,368,96,22);
            add(L"BUTTON",L"清理不兼容订单",BS_OWNERDRAW|WS_TABSTOP,market::CleanInvalid,32,396,195,38);
            add(L"BUTTON",L"清空在售订单",BS_OWNERDRAW|WS_TABSTOP,market::ClearOrders,239,396,195,38);
            add(L"BUTTON",L"清空均价数据",BS_OWNERDRAW|WS_TABSTOP,market::ClearPrices,446,396,195,38);
            add(L"BUTTON",L"重置拍卖库",BS_OWNERDRAW|WS_TABSTOP,market::Reset,653,396,195,38);
            for(int id:std::initializer_list<int>{325,322,MARKET_INDEX_LABEL,MARKET_DATA_LABEL})
                SendMessageW(GetDlgItem(w,id),WM_SETFONT,reinterpret_cast<WPARAM>(app->strong_font),TRUE);
            app->show_plugins(); app->show_level(); app->controls();
            market_summary();
            app->market_tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
                CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,w,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(app->market_tooltip!=nullptr,"Create PVF path tooltip");
            TOOLINFOW info{}; info.cbSize=sizeof(info); info.uFlags=TTF_IDISHWND|TTF_SUBCLASS; info.hwnd=w;
            info.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(w,MARKET_SUMMARY)); info.lpszText=app->market_pvf_text.data();
            SendMessageW(app->market_tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
            SendMessageW(app->market_tooltip,TTM_SETMAXTIPWIDTH,0,app->px(760));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc=BeginPaint(w,&paint); RECT area; GetClientRect(w,&area);
            FillRect(dc,&area,app->background);
            auto panel=[&](int left,int top,int right,int bottom,COLORREF fill,COLORREF border) {
                RECT r={app->px(left),app->px(top),app->px(right),app->px(bottom)};
                rounded(dc,r,fill,border,app->px(12));
            };
            panel(16,12,864,64,RGB(247,250,255),RGB(222,231,245));
            panel(16,72,864,156,RGB(255,255,255),RGB(228,234,245));
            panel(16,164,864,274,RGB(255,255,255),RGB(228,234,245));
            panel(16,282,864,446,RGB(255,255,255),RGB(228,234,245));
            EndPaint(w,&paint); return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc=reinterpret_cast<HDC>(wp);
            // The summary keeps the banner tint so the read-only text block
            // does not punch a white rectangle out of the rounded panel.
            if(GetDlgCtrlID(reinterpret_cast<HWND>(lp))==MARKET_SUMMARY || GetDlgCtrlID(reinterpret_cast<HWND>(lp))==MARKET_STATUS) {
                SetBkColor(dc,RGB(247,250,255)); SetTextColor(dc,RGB(54,77,112));
                return reinterpret_cast<LRESULT>(app->banner);
            }
            SetBkColor(dc,RGB(255,255,255));
            return reinterpret_cast<LRESULT>(app->surface);
        }
        case WM_ERASEBKGND: { RECT r;GetClientRect(w,&r);FillRect(reinterpret_cast<HDC>(wp),&r,app->background);return 1; }
        case WM_DRAWITEM: draw_item(*reinterpret_cast<DRAWITEMSTRUCT *>(lp)); return TRUE;
        case WM_COMMAND: {
            int id=LOWORD(wp);
            if(id==IDCANCEL) { SendMessageW(w,WM_CLOSE,0,0); return 0; }
            if(id==DP2_ENABLE || id==FRIDA_ENABLE || id==LEVEL_CAP || id==LEVEL_APPLY) {
                SendMessageW(app->window,WM_COMMAND,wp,lp);return 0;
            }
            if(id>=market::Check && id<=market::Reset && HIWORD(wp)==BN_CLICKED && !app->busy) {
                app->action(id,L"",2); return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            if(app->busy) return 0;
            DestroyWindow(w); return 0;
        case WM_DESTROY:
            if(app->market_tooltip) DestroyWindow(app->market_tooltip);
            app->market_tooltip=nullptr; app->market_window=nullptr;
            EnableWindow(app->window,TRUE); SetActiveWindow(app->window); app->controls(); return 0;
        }
    } catch(const std::exception &e) {
        std::wstring message=L"拍卖维护失败："+decode(e.what());
        app->notice(message);
    }
    return DefWindowProcW(w,message,wp,lp);
}
static void open_market_window() {
    if(app->market_window) { SetForegroundWindow(app->market_window); return; }
    WNDCLASSW cls{}; cls.lpfnWndProc=market_proc; cls.hInstance=GetModuleHandleW(nullptr); cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName=L"DnfNativeMarketMaintenance";
    if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) require(false,"Register market maintenance window");
    RECT size{0,0,app->px(880),app->px(460)}; AdjustWindowRectEx(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_CONTROLPARENT);
    RECT owner;GetWindowRect(app->window,&owner);
    HWND w=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"功能",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        std::max(0L,owner.left+(owner.right-owner.left-(size.right-size.left))/2),std::max(0L,owner.top),size.right-size.left,size.bottom-size.top,app->window,nullptr,cls.hInstance,nullptr);
    require(w!=nullptr,"Open feature window"); EnableWindow(app->window,FALSE); ShowWindow(w,SW_SHOW);
}
