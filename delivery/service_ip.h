// Single-host service addresses. Database endpoints and monitoring bypass stay local.
#pragma once

static bool valid_ipv4(const std::wstring &value) {
    IN_ADDR address = {};
    return InetPtonW(AF_INET, value.c_str(), &address) == 1 &&
           address.s_addr != INADDR_ANY && address.s_addr != INADDR_BROADCAST;
}

static bool service_address_key(const std::string &key) {
    return key == "this_ip" || key == "bridge_ip" || key == "monitor_ip" || key == "ip" ||
           key.rfind("udp_ip_of_", 0) == 0 || key == "ipg_ip" || key == "nxj_ipg_ip" ||
           key == "relay_ip" || key == "logserver_ip" || key == "stun_ip" ||
           key == "exchange_server_ip" || key == "auction_server_ip" ||
           key == "cera_auction_server_ip" || key == "community_server_ip" || key == "monitor_server_ip";
}

static std::string replace_service_addresses(const std::string &text, const std::string &ip) {
    std::string result;
    size_t offset = 0;
    while (offset < text.size()) {
        size_t end = text.find('\n',offset);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(offset,end-offset);
        size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line[first] != '#' && line[first] != ';' && line[first] != '/') {
            size_t equal = line.find('=',first);
            if (equal != std::string::npos) {
                size_t key_end = line.find_last_not_of(" \t",equal-1);
                if (service_address_key(line.substr(first,key_end-first+1))) {
                    size_t begin = line.find_first_not_of(" \t",equal+1);
                    size_t finish = begin == std::string::npos ? begin : line.find_first_of(" \t\r;#",begin);
                    if (begin != std::string::npos) line.replace(begin,(finish == std::string::npos ? line.size() : finish)-begin,ip);
                }
            } else {
                // Original guild/monitor use positional records rather than INI fields.
                std::string kind = line.substr(first,line.find_first_of(" \t",first)-first);
                if (kind == "5.Define_DBMW_TCP_IP" || kind == "7.Define_MANAGER_TCP_IP" ||
                    kind == "DBMW" || kind == "Manager" || kind == "Relay") {
                    size_t begin = first;
                    while (begin < line.size()) {
                        size_t finish = line.find_first_of(" \t\r",begin);
                        if (finish == std::string::npos) finish = line.size();
                        std::string token = line.substr(begin,finish-begin);
                        if (valid_ipv4(std::wstring(token.begin(),token.end()))) { line.replace(begin,finish-begin,ip); break; }
                        begin = line.find_first_not_of(" \t\r",finish);
                    }
                }
            }
        }
        result += line;
        if (end < text.size()) result += '\n';
        offset = end + 1;
    }
    return result;
}

static std::vector<std::wstring> service_configs(const std::vector<Service> &services) {
    std::vector<std::wstring> paths;
    for (const auto &service : services) {
        std::wstring folder = service.root + service.cwd + L"\\cfg";
        WIN32_FIND_DATAW item = {};
        HANDLE search = FindFirstFileW((folder + L"\\*.cfg").c_str(),&item);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) paths.push_back(folder + L"\\" + item.cFileName);
        } while (FindNextFileW(search,&item));
        FindClose(search);
    }
    std::sort(paths.begin(),paths.end());
    paths.erase(std::unique(paths.begin(),paths.end()),paths.end());
    return paths;
}

static unsigned update_service_ip(const std::vector<Service> &services, const std::wstring &instance, const std::wstring &value) {
    if (!valid_ipv4(value)) throw std::runtime_error("Service IP must be a usable IPv4 address");
    struct Change { std::wstring path, backup; std::string text; };
    std::vector<Change> changes;
    std::string ip(value.begin(),value.end());
    std::wstring backup = instance + L"\\.settings\\ip-" + std::to_wstring(GetTickCount64());
    for (const auto &path : service_configs(services)) {
        std::string old = read_file(path), updated = replace_service_addresses(old,ip);
        if (updated != old) changes.push_back({path,backup + L"\\" + std::to_wstring(changes.size()) + L".cfg",updated});
    }
    if (changes.empty()) return 0;
    directory(backup);
    std::string index;
    for (const auto &change : changes) {
        require(CopyFileW(change.path.c_str(),change.backup.c_str(),TRUE),"Back up service IP configuration");
        // Paths are stored privately beside the originals for manual inspection.
        int bytes = WideCharToMultiByte(CP_UTF8,0,change.path.c_str(),-1,nullptr,0,nullptr,nullptr);
        std::string path(bytes,'\0'); WideCharToMultiByte(CP_UTF8,0,change.path.c_str(),-1,path.data(),bytes,nullptr,nullptr);
        index += path.c_str(); index += '\n';
    }
    write_private(backup + L"\\paths.txt",index);
    size_t written = 0;
    try {
        for (const auto &change : changes) {
            write_private(change.path + L".ip-new",change.text);
            require(MoveFileExW((change.path + L".ip-new").c_str(),change.path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Apply service IP configuration");
            ++written;
        }
    } catch (...) {
        bool restored = true;
        for (size_t i=0;i<written;++i) restored = CopyFileW(changes[i].backup.c_str(),changes[i].path.c_str(),FALSE) && restored;
        if (!restored) throw std::runtime_error("Service IP update failed and rollback was incomplete; inspect .settings backup");
        throw;
    }
    return static_cast<unsigned>(changes.size());
}

static std::wstring configured_service_ip(const std::vector<Service> &services) {
    for (const auto &service : services) if (service.name == L"channel") {
        std::wstring ip = setting(service.root + service.cwd + L"\\cfg\\channel.cfg",L"server",L"this_ip");
        if (valid_ipv4(ip)) return ip;
    }
    return L"127.0.0.1";
}
