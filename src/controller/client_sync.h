// Optional client import. Existing files are backed up before any replacement.
#pragma once
namespace client_sync {
struct Status { int pvf=0,keys=0; };
inline std::wstring selected(const std::wstring &instance) {return setting(instance+L"\\services.ini",L"client",L"directory");}
inline std::wstring private_path(const std::wstring &client) {
    return present(client+L"\\private_key.pem")?client+L"\\private_key.pem":client+L"\\privatekey.pem";
}
inline std::string client_private(const std::wstring &client) {
    auto path=private_path(client);
    return present(path)?read_file(path):default_game_private_key();
}
inline Status inspect(const std::wstring &instance,const std::vector<Service> &services) {
    Status result;auto client=selected(instance);if(client.empty()) return result;
    result.pvf=result.keys=3;
    try {result.pvf=file_sha256(client+L"\\Script.pvf")==file_sha256(market::game_pvf(services))?1:2;}catch(...) {}
    try {
        auto pub=key_der(game_public_key(read_file(private_path(client))));
        auto game=game_directory(instance);
        result.keys=pub==key_der(read_file(game+L"\\publickey.pem")) &&
            pub==key_der(game_public_key(read_file(game+L"\\privatekey.pem")))?1:2;
        for(auto name:{L"publickey.pem",L"public_key.pem"})
            if(present(client+L"\\"+name) && key_der(read_file(client+L"\\"+name))!=pub) result.keys=2;
    }catch(...) {}
    return result;
}
inline std::wstring signature(const std::wstring &instance,const std::vector<Service> &services) {
    auto client=selected(instance);if(client.empty()) return {};
    std::wstring result=client;
    auto game=game_directory(instance);
    for(auto path:{client+L"\\Script.pvf",market::game_pvf(services),client+L"\\private_key.pem",client+L"\\privatekey.pem",
                   client+L"\\publickey.pem",client+L"\\public_key.pem",game+L"\\publickey.pem",game+L"\\privatekey.pem"}) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a)) result+=L"|"+std::to_wstring(a.nFileSizeHigh)+L":"+std::to_wstring(a.nFileSizeLow)+L":"+std::to_wstring(log_time(a.ftLastWriteTime));
        else result+=L"|missing";
    }
    return result;
}
inline std::wstring import(const std::wstring &instance,const std::vector<Service> &services,const std::wstring &client) {
    for(const auto &s:services) if(running(s)) throw std::runtime_error("Stop services before importing client files");
    auto source=client+L"\\Script.pvf",target=market::game_pvf(services);
    auto hash=file_sha256(source); // Reject missing/inaccessible input before writes.
    auto secret=client_private(client),pub=game_public_key(secret),der=key_der(pub);
    for(auto name:{L"private_key.pem",L"privatekey.pem"})
        if(present(client+L"\\"+name) && key_der(game_public_key(read_file(client+L"\\"+name)))!=der)
            throw std::runtime_error("Client private keys conflict; files preserved");
    for(auto name:{L"publickey.pem",L"public_key.pem"})
        if(present(client+L"\\"+name) && key_der(read_file(client+L"\\"+name))!=der)
            throw std::runtime_error("Client public/private keys do not match; files preserved");
    auto backup=instance+L"\\.mysql\\client-sync-"+unique_name();directory(backup);
    struct Change {std::wstring path,old,next;bool existed=false,committed=false;};
    std::vector<Change> changes;
    auto stage=[&](const std::wstring &path,const std::string *bytes,bool pvf=false) {
        if((!pvf && present(path) && read_file(path)==*bytes) || (pvf && present(path) && file_sha256(path)==hash)) return;
        auto label=std::to_wstring(changes.size());Change c{path,backup+L"\\"+label+L".before",path+L".sync-"+unique_name()};
        c.existed=present(path);
        if(c.existed) {
            if(pvf) {require(CopyFileW(path.c_str(),c.old.c_str(),TRUE),"Back up instance PVF");if(file_sha256(c.old)!=file_sha256(path)) throw std::runtime_error("PVF backup verification failed");}
            else write_private(c.old,read_file(path));
        }
        changes.push_back(c);
        if(pvf) {require(CopyFileW(source.c_str(),c.next.c_str(),TRUE),"Stage client PVF");if(file_sha256(c.next)!=hash || file_sha256(source)!=hash) throw std::runtime_error("Client PVF changed during import");}
        else write_private(c.next,*bytes);
    };
    try {
        stage(target,nullptr,true);
        auto game=game_directory(instance);
        stage(game+L"\\privatekey.pem",&secret);stage(game+L"\\publickey.pem",&pub);
        if(!present(client+L"\\private_key.pem")) stage(client+L"\\private_key.pem",&secret);
        if(!present(client+L"\\publickey.pem")) stage(client+L"\\publickey.pem",&pub);
        // Keep the instance association in the same rollback set as the files.
        auto config=instance+L"\\services.ini",next=backup+L"\\services.ini";
        require(CopyFileW(config.c_str(),next.c_str(),TRUE),"Stage client association");
        require(WritePrivateProfileStringW(L"client",L"directory",client.c_str(),next.c_str()),"Save client directory");
        auto bytes=read_file(next);stage(config,&bytes);
        std::string manifest;
        for(const auto &c:changes)
            manifest+=utf8_path(c.path)+"\t"+(c.existed?utf8_path(c.old):"new file")+"\n";
        write_private(backup+L"\\files.private.txt",manifest);
        for(auto &c:changes) {
            require(MoveFileExW(c.next.c_str(),c.path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Commit client synchronization");c.committed=true;
        }
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,(instance+L"\\services.ini").c_str());
        auto result=inspect(instance,services);
        if(result.pvf!=1 || result.keys!=1) throw std::runtime_error("Client synchronization verification failed");
    }catch(...) {
        bool restored=true;
        for(auto c=changes.rbegin();c!=changes.rend();++c) {
            if(c->committed) {
                if(c->existed) restored=CopyFileW(c->old.c_str(),c->path.c_str(),FALSE)!=FALSE && restored;
                else restored=DeleteFileW(c->path.c_str())!=FALSE && restored;
            }
            if(present(c->next)) DeleteFileW(c->next.c_str());
        }
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,(instance+L"\\services.ini").c_str());
        if(!restored) throw std::runtime_error("Client import rollback incomplete; preserve client-sync backup and inspect file permissions");
        throw;
    }
    return L"客户端 PVF 与密钥已同步。\r\n客户端："+client+L"\r\n备份："+backup;
}
}
