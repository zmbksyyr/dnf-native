// Controller-owned maintenance for the two original auction databases.
#pragma once
#include "auction_pvf.h"
#include <functional>
#include <exception>

namespace market {
enum Operation { Check=310, Sync, RestoreIndex, Apply, CleanInvalid, ClearOrders, ClearPrices, Reset };
struct Options { bool levels=true,refine=true,dp2=false,automatic=true; };
inline Options options(const std::wstring &instance) {
    auto f=instance+L"\\services.ini";
    return {setting(f,L"auction",L"extend_levels")!=L"0",setting(f,L"auction",L"extend_refine")!=L"0",
            setting(f,L"auction",L"dp2_guard")==L"1",setting(f,L"auction",L"auto_check")!=L"0"};
}
inline void save_options(const std::wstring &instance,const Options &o) {
    auto f=instance+L"\\services.ini";
    for(auto p:{std::pair<const wchar_t *,bool>{L"extend_levels",o.levels},{L"extend_refine",o.refine},{L"dp2_guard",o.dp2},{L"auto_check",o.automatic}})
        require(WritePrivateProfileStringW(L"auction",p.first,p.second?L"1":L"0",f.c_str()),"Save auction options");
}
inline std::wstring game_pvf(const std::vector<Service> &services) {
    for(const auto &s:services) if(s.name==L"game") return s.root+s.cwd+L"\\Script.pvf";
    throw std::runtime_error("Game PVF path is missing");
}
inline std::vector<std::wstring> indexes(const std::vector<Service> &services) {
    std::vector<std::wstring> out;
    for(const auto *name:{L"auction",L"point"}) for(const auto &s:services) if(s.name==name) out.push_back(s.root+s.cwd+L"\\iteminfo.dat");
    if(out.size()!=2) throw std::runtime_error("Both market services must be configured"); return out;
}
inline std::string query(const std::wstring &exe,const std::wstring &instance,const std::wstring &text) {
    auto r=sql(exe,instance,text.c_str()); if(r.code) throw std::runtime_error("Auction SQL failed; private log retained");
    r.output.erase(std::remove(r.output.begin(),r.output.end(),'\r'),r.output.end()); return r.output;
}
inline std::vector<std::wstring> databases(int scope) {
    if(scope<0 || scope>2) throw std::runtime_error("Select a market database scope");
    std::vector<std::wstring> out;
    if(scope!=1) out.push_back(L"taiwan_cain_auction_gold");
    if(scope!=0) out.push_back(L"taiwan_cain_auction_cera"); return out;
}
inline bool index_matches(const std::wstring &,const std::vector<std::wstring> &,const std::string &);
inline std::vector<std::wstring> tables(const std::wstring &exe,const std::wstring &instance,const std::wstring &db) {
    std::vector<std::wstring> out; std::istringstream lines(query(exe,instance,L"SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA='"+db+L"' AND TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME;"));
    std::string s;
    while(std::getline(lines,s)) {
        if(s.empty() || s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)
            throw std::runtime_error("Unexpected auction table identifier");
        out.emplace_back(s.begin(),s.end());
    }
    if(out.empty()) throw std::runtime_error("Auction database is not initialized"); return out;
}
struct Conflict { std::wstring db; std::string order; int item; std::string reason; };
struct Inspection {
    std::vector<Conflict> conflicts;
    std::vector<std::string> price_conflicts;
    unsigned orders=0,prices=0;
    std::wstring report;
};
inline Inspection inspect(const std::wstring &exe,const std::wstring &instance,const Catalog &c,
                          const std::vector<std::wstring> &paths,const Options &o,int scope) {
    Inspection out; std::map<int,Fields> old[2];
    std::map<int,std::string> identities;
    auto identity_file=instance+L"\\.mysql\\market-definitions.txt";
    if(present(identity_file)) {
        std::istringstream lines(read_file(identity_file)); std::string line;
        while(std::getline(lines,line)) { auto tab=line.find('\t'); if(tab!=std::string::npos) identities[std::stoi(line.substr(0,tab))]=line.substr(tab+1); }
    }
    for(unsigned i=0;i<paths.size();++i) old[i]=parse_dat(read_file(paths[i]));
    for(const auto &db:databases(scope)) {
        unsigned which=db==L"taiwan_cain_auction_gold" ? 0:1;
        auto incompatible=[&](int item,unsigned upgrade,unsigned refine) {
            std::string reason; auto d=c.items.find(item);
            if(d==c.items.end()) reason="ID is absent from current PVF";
            else if(!d->second.reason.empty()) reason=d->second.reason;
            else if(upgrade>31) reason="upgrade exceeds original protocol limit31";
            else if(refine>(o.refine?c.max_refine:7)) reason="refine exceeds selected PVF/runtime limit";
            else if(std::stoul(d->second.fields[13])>(o.levels?c.max_level:70)) reason="level exceeds selected search limit";
            else if(identities.count(item) && identities.at(item)!=d->second.signature) reason="PVF definition/path/checksum changed since last synchronization";
            else if(old[which].count(item)) {
                auto &a=old[which].at(item); auto &b=d->second.fields;
                for(unsigned k:{1u,13u,16u}) if(a[k]!=b[k]) reason="definition rarity/level/category changed";
                if(a[14]!=b[14]) {
                    bool same=false;
                    for(UINT cp:{936u,950u}) try { same=same || utf8(wide(a[14],cp))==b[14]; } catch(...) {}
                    if(!same) reason="definition name changed or ID was reused";
                }
            } else reason="existing index has no definition for stored reference";
            return reason;
        };
        // The caller supplies indexes in auction,point order.
        std::istringstream rows(query(exe,instance,L"SELECT auction_id,IFNULL(item_id,0),IFNULL(upgrade,0),seperate_upgrade FROM "+db+L".auction_main ORDER BY auction_id;"));
        std::string line;
        while(std::getline(rows,line)) {
            std::istringstream row(line); std::string order; int item; unsigned upgrade,refine;
            if(!(row>>order>>item>>upgrade>>refine) || order.find_first_not_of("0123456789")!=std::string::npos)
                throw std::runtime_error("Auction order projection has an unknown layout");
            ++out.orders; auto reason=incompatible(item,upgrade,refine);
            if(!reason.empty()) out.conflicts.push_back({db,order,item,reason});
        }
        // Prices also survive restart. An ID reused by another PVF must not
        // silently inherit an old normal, ROI or manually supplied price.
        for(const auto *table:{L"auction_average_price",L"auction_roi_average_price",L"auction_manual_average_price"}) {
            std::wstring t=table;
            bool has_refine=query(exe,instance,L"SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='"+db+L"' AND TABLE_NAME='"+t+L"' AND COLUMN_NAME='seperate_upgrade';")!="0\n";
            std::istringstream rows(query(exe,instance,L"SELECT item_id,upgrade,"+(has_refine?std::wstring(L"seperate_upgrade"):std::wstring(L"0"))+L",COUNT(*) FROM "+db+L"."+t+L" GROUP BY item_id,upgrade"+(has_refine?std::wstring(L",seperate_upgrade"):std::wstring())+L";"));
            while(std::getline(rows,line)) {
                std::istringstream row(line); int item; unsigned upgrade,refine,count;
                if(!(row>>item>>upgrade>>refine>>count)) throw std::runtime_error("Auction price projection has an unknown layout");
                out.prices+=count; auto reason=incompatible(item,upgrade,refine);
                if(!reason.empty()) out.price_conflicts.push_back(std::string(db.begin(),db.end())+"."+std::string(t.begin(),t.end())+" / item "+std::to_string(item)+" / upgrade "+std::to_string(upgrade)+" / refine "+std::to_string(refine)+" ("+std::to_string(count)+"): "+reason);
            }
        }
    }
    out.report=L"PVF SHA256："+decode(c.pvf_hash)+L"\r\n编码："+(c.encoding==936?std::wstring(L"GBK"):std::wstring(L"Big5"))+
        L" → UTF-8 索引；装备等级上限 "+std::to_wstring(c.max_level)+L"，锻造上限 "+std::to_wstring(c.max_refine)+
        L"\r\n可索引定义 "+std::to_wstring(c.items.size()-c.excluded)+L"，待处理定义 "+std::to_wstring(c.excluded)+
        L"；存量订单 "+std::to_wstring(out.orders)+L"，不兼容订单 "+std::to_wstring(out.conflicts.size())+L"\r\n";
    out.report+=L"存量均价记录 "+std::to_wstring(out.prices)+L"，不兼容均价组 "+std::to_wstring(out.price_conflicts.size())+L"（可用红字“清空均价数据”处理）\r\n";
    out.report+=index_matches(instance,paths,c.pvf_hash)?L"索引匹配：两市场已同步当前 PVF\r\n":L"索引匹配：尚未同步，或文件/PVF 已变化\r\n";
    unsigned displayed=0;
    for(const auto &v:c.items) if(!v.second.reason.empty() && displayed++<12)
        out.report+=L"待处理定义 "+std::to_wstring(v.first)+L"："+decode(v.second.reason)+L"\r\n";
    for(const auto &v:out.conflicts) out.report+=v.db+L" #"+decode(v.order)+L" / item "+std::to_wstring(v.item)+L"："+decode(v.reason)+L"\r\n";
    for(const auto &v:out.price_conflicts) out.report+=decode(v)+L"\r\n";
    return out;
}
struct Backup { std::wstring folder; std::vector<std::pair<std::wstring,std::string>> dumps; };
inline Backup backup(const std::wstring &exe,const std::wstring &instance,int scope) {
    Backup b; b.folder=instance+L"\\.mysql\\market-"+unique_name(); directory(b.folder);
    for(const auto &db:databases(scope)) {
        auto ts=tables(exe,instance,db); auto path=b.folder+L"\\"+db+L".sql";
        write_private(path,"");
        auto r=run_tool(exe,instance,{L"mysqldump",instance,L"--defaults-file=client.private.cnf",L"--lock-tables",L"--hex-blob",L"--skip-comments",L"--set-gtid-purged=OFF",L"--result-file="+path,L"--databases",db},180000);
        if(r.code) throw std::runtime_error("Auction backup failed; data is unchanged");
        auto text=read_file(path);
        for(const auto &t:ts) if(text.find("CREATE TABLE `"+std::string(t.begin(),t.end())+"`")==std::string::npos)
            throw std::runtime_error("Auction backup lacks a required table; data is unchanged");
        auto hash=file_sha256(path); write_private(path+L".sha256",hash+"\n"); b.dumps.emplace_back(path,hash);
    }
    return b;
}
inline void rollback(const std::wstring &exe,const std::wstring &instance,const Backup &b) {
    for(const auto &d:b.dumps) {
        if(file_sha256(d.first)!=d.second) throw std::runtime_error("Auction recovery backup hash differs; services remain stopped");
        if(run_tool(exe,instance,{L"mysql",instance,L"--defaults-file=client.private.cnf"},180000,d.first).code)
            throw std::runtime_error("Auction rollback failed; backup and private logs retained; services remain stopped");
    }
}
inline void replace_file(const std::wstring &path,const std::string &bytes) {
    auto next=path+L".next-"+unique_name(); write_private(next,bytes);
    require(MoveFileExW(next.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Commit auction file");
}
inline void save_limits(const std::wstring &instance,const Catalog &c,const Options &o) {
    auto f=instance+L"\\services.ini";
    require(WritePrivateProfileStringW(L"auction",L"level_limit",std::to_wstring(o.levels?c.max_level:70).c_str(),f.c_str()),"Save market level limit");
    require(WritePrivateProfileStringW(L"auction",L"refine_limit",std::to_wstring(o.refine?c.max_refine:7).c_str(),f.c_str()),"Save market refine limit");
}
inline void price_schema(const std::wstring &exe,const std::wstring &instance,int scope) {
    for(const auto &db:databases(scope)) {
        for(const auto *table:{L"auction_average_price",L"auction_roi_average_price"}) {
            std::wstring t=table;
            auto name=t==L"auction_average_price" ? L"PRIMARY":L"item_id";
            std::string expected=t==L"auction_average_price" ? "item_id,upgrade" : "item_id,upgrade,roi_high_key,roi_low_key,roi_index1,roi_index2,roi_index3";
            auto current=query(exe,instance,L"SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA='"+db+L"' AND TABLE_NAME='"+t+L"' AND INDEX_NAME='"+name+L"';");
            if(current==expected+",seperate_upgrade\n") continue;
            if(current!=expected+"\n") throw std::runtime_error("Unknown auction price unique key; backup retained, no guessed migration");
            query(exe,instance,L"ALTER TABLE "+db+L"."+t+(t==L"auction_average_price"?L" DROP PRIMARY KEY, ADD PRIMARY KEY (":L" DROP INDEX item_id, ADD UNIQUE KEY item_id (")+decode(expected)+L",seperate_upgrade);");
        }
        if(query(exe,instance,L"SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='"+db+L"' AND TABLE_NAME='auction_manual_average_price' AND COLUMN_NAME='seperate_upgrade';")=="0\n")
            query(exe,instance,L"ALTER TABLE "+db+L".auction_manual_average_price ADD COLUMN seperate_upgrade TINYINT UNSIGNED NOT NULL DEFAULT 0;");
    }
}
inline bool index_matches(const std::wstring &instance,const std::vector<std::wstring> &paths,const std::string &pvf) {
    auto f=instance+L"\\.mysql\\market-index.ini";
    if(setting(f,L"index",L"pvf")!=decode(pvf)) return false;
    for(unsigned i=0;i<paths.size();++i) if(setting(f,L"index",L"file"+std::to_wstring(i))!=decode(file_sha256(paths[i]))) return false;
    return true;
}
inline void index_manifest(const std::wstring &instance,const std::vector<std::wstring> &paths,const Catalog &c) {
    std::string s="[index]\r\npvf="+c.pvf_hash+"\r\nencoding=UTF8\r\n";
    for(unsigned i=0;i<paths.size();++i) s+="file"+std::to_string(i)+"="+file_sha256(paths[i])+"\r\n";
    replace_file(instance+L"\\.mysql\\market-index.ini",s);
}
inline std::string dp2_search_block() {
    return R"JS(    // DNF Native: preserve ordinary item bytes; replace only validated socket records.
    var nativeAuctionDepth = 0;
    var Inter_AuctionResultItemList_dispatch_sig = new NativeFunction(ptr(0x084D75BC), 'int', ['pointer', 'pointer', 'pointer', 'int'], {abi:'sysv'});
    Interceptor.replace(ptr(0x084D75BC), new NativeCallback(function (self, user, packet, size) {
        if (nativeAuctionDepth) return Inter_AuctionResultItemList_dispatch_sig(self, user, packet, size);
        ++nativeAuctionDepth;
        try {
            var count = packet.add(5).readU8();
            for (var i = 0; i < count; ++i) {
                var entry = packet.add(i * 137), itemId = entry.add(54).readU32(), instanceId = entry.add(76).readU32();
                if (itemId <= 1 || instanceId <= 1) continue;
                var item = CDataManager_find_item(G_CDataManager(), itemId);
                if (item.isNull()) continue;
                var group = CItem_getItemGroupName(item);
                if (group < 1 || group > 58) continue;
                if (api_MySQL_exec(mysql_frida, 'SELECT HEX(jewel_data) FROM data WHERE equ_id=' + instanceId + ';') !== 1) continue;
                if (MySQL_get_n_rows(mysql_frida) !== 1 || !MySQL_fetch(mysql_frida)) continue;
                var hex = api_MySQL_get_str(mysql_frida, 0);
                if (!hex || !/^[0-9a-fA-F]{60}$/.test(hex)) continue;
                var bytes = [];
                for (var j = 0; j < 60; j += 2) bytes.push(parseInt(hex.substr(j, 2), 16));
                // A valid record must advertise at least one actual socket.
                if (bytes[0] === 0) continue;
                entry.add(106).writeByteArray(bytes);
            }
        } catch (error) {
            console.error('DNF Native auction socket protection: ' + error);
        } finally { --nativeAuctionDepth; }
        return Inter_AuctionResultItemList_dispatch_sig(self, user, packet, size);
    }, 'int', ['pointer', 'pointer', 'pointer', 'int']));
)JS";
}
inline std::string dp2_script(const std::wstring &assets,const std::wstring &instance,bool enabled) {
    auto seed=instance+L"\\.mysql\\market-dp2-seed.js";
    if(!present(seed)) ResourceZip(assets+L"\\native.zip").extract({{"dp2/df_game_r.js",seed}},false);
    auto base=read_file(seed); const std::string begin="    var Inter_AuctionResultItemList_dispatch_sig =",end="    var Inter_AuctionResultMyBidding_dispatch_sig =";
    size_t a=base.find(begin),b=base.find(end,a);
    if(a==std::string::npos || b==std::string::npos || base.find(begin,a+1)!=std::string::npos) throw std::runtime_error("Packaged DP2 search hook identity differs");
    auto path=instance+L"\\dp2\\df_game_r.js"; auto current=read_file(path);
    auto old_block=base.substr(a,b-a),new_block=dp2_search_block();
    auto old=current.find(old_block),ours=current.find(new_block);
    // Instance setup legitimately changes database credentials outside this
    // hook. Preserve all surrounding user code and check only the replaced span.
    if(ours!=std::string::npos) {
        if(enabled) return current;
        return current.substr(0,ours)+old_block+current.substr(ours+new_block.size());
    }
    if(!enabled) return current;
    if(old==std::string::npos || current.find(old_block,old+1)!=std::string::npos)
        throw std::runtime_error("DP2 search hook is customized; automatic protection is unavailable");
    return current.substr(0,old)+new_block+current.substr(old+old_block.size());
}
inline std::wstring maintain(const std::wstring &exe,const std::wstring &instance,const std::wstring &assets,
    std::vector<Service> &services,HANDLE job,int operation,int scope,const Options &o,const std::function<void()> &identity) {
    identity(); auto pvf=game_pvf(services); auto paths=indexes(services); auto c=catalog(pvf);
    auto check=inspect(exe,instance,c,paths,o,scope);
    std::string details;
    for(const auto &v:c.items) if(!v.second.reason.empty()) details+=std::to_string(v.first)+"\t"+v.second.reason+"\t"+v.second.signature+"\n";
    auto report_file=instance+L"\\.mysql\\market-check-"+unique_name()+L".private.txt";
    write_private(report_file,market::utf8(check.report)+"\nUnsupported definitions:\n"+details);
    check.report+=L"完整定义报告："+report_file+L"\r\n";
    if(operation==Check) return check.report;
    auto restore_scope=databases(scope);
    std::vector<size_t> restart;
    for(size_t i=0;i<services.size();++i) if(running(services[i]) &&
        (services[i].name==L"game" || services[i].name==L"auction" || services[i].name==L"point")) restart.push_back(i);
    // Stop writers before the authoritative inspection and the backup.
    for(auto i=restart.rbegin();i!=restart.rend();++i) stop_service(services[*i]);
    identity(); check=inspect(exe,instance,c,paths,o,scope);
    auto config=read_file(instance+L"\\services.ini");
    auto index_ini=instance+L"\\.mysql\\market-index.ini";
    std::string old_manifest=present(index_ini)?read_file(index_ini):"";
    auto definitions=instance+L"\\.mysql\\market-definitions.txt";
    std::string old_definitions=present(definitions)?read_file(definitions):"";
    std::vector<std::string> original; for(const auto &p:paths) original.push_back(read_file(p));
    std::wstring dp2_path=instance+L"\\dp2\\df_game_r.js";
    std::string old_js=present(dp2_path)?read_file(dp2_path):"";
    Backup b; bool changed=false;
    try {
        if((operation==Sync || operation==Apply) && (!check.conflicts.empty() || !check.price_conflicts.empty())) throw std::runtime_error("Stored orders/prices conflict with current PVF/options; run compatibility check and use the matching red cleanup action if intended");
        if(operation==Sync || operation==Apply || operation==RestoreIndex) {
            b=backup(exe,instance,2);
            for(unsigned i=0;i<paths.size();++i) {
                auto f=b.folder+L"\\index"+std::to_wstring(i)+L".dat"; write_private(f,original[i]); write_private(f+L".sha256",file_sha256(f));
            }
            write_private(b.folder+L"\\services.ini",config);
            write_private(b.folder+L"\\index-manifest.ini",old_manifest);
            write_private(b.folder+L"\\definitions.txt",old_definitions);
            if(!old_js.empty()) write_private(b.folder+L"\\dp2.js",old_js);
            // The old index belongs to its recorded PVF, which may differ from
            // the newly selected PVF. Never label an untracked old index as new.
            auto previous_pvf=setting(index_ini,L"index",L"pvf");
            write_private(b.folder+L"\\pvf.sha256",std::string(previous_pvf.begin(),previous_pvf.end()));
            changed=true;
            price_schema(exe,instance,2);
            if(operation==RestoreIndex) {
                auto previous=setting(instance+L"\\services.ini",L"auction",L"previous_index");
                if(previous.empty() || read_file(previous+L"\\pvf.sha256")!=c.pvf_hash)
                    throw std::runtime_error("Previous index belongs to a different PVF; restore its PVF first");
                for(unsigned i=0;i<paths.size();++i) {
                    auto f=previous+L"\\index"+std::to_wstring(i)+L".dat";
                    if(file_sha256(f)!=read_file(f+L".sha256")) throw std::runtime_error("Previous iteminfo backup hash differs");
                    replace_file(paths[i],read_file(f));
                }
                // Restore only market settings; an index rollback must preserve
                // later changes to service addresses, game levels and user code.
                for(const auto *key:{L"extend_levels",L"extend_refine",L"dp2_guard",L"auto_check",L"level_limit",L"refine_limit"}) {
                    auto value=setting(previous+L"\\services.ini",L"auction",key);
                    require(WritePrivateProfileStringW(L"auction",key,value.empty()?nullptr:value.c_str(),(instance+L"\\services.ini").c_str()),"Restore market setting");
                }
                auto m=read_file(previous+L"\\index-manifest.ini");
                if(!m.empty()) replace_file(index_ini,m); else if(present(index_ini)) require(DeleteFileW(index_ini.c_str()),"Remove restored legacy index marker");
                if(!old_js.empty()) replace_file(dp2_path,dp2_script(assets,instance,options(instance).dp2));
                auto ids=read_file(previous+L"\\definitions.txt");
                if(!ids.empty()) replace_file(definitions,ids); else if(present(definitions)) require(DeleteFileW(definitions.c_str()),"Restore original market definitions marker");
                auto restored=inspect(exe,instance,c,paths,options(instance),2);
                if(!restored.conflicts.empty() || !restored.price_conflicts.empty()) throw std::runtime_error("Previous index no longer matches stored orders/prices");
            } else {
                auto dat=c.dat(); if(dat.empty()) throw std::runtime_error("PVF has no supported searchable definitions");
                for(const auto &p:paths) replace_file(p,dat);
                if(present(dp2_path)) replace_file(dp2_path,dp2_script(assets,instance,o.dp2));
                else if(o.dp2) throw std::runtime_error("DP2 search protection requires the bundled Frida script");
                save_options(instance,o); save_limits(instance,c,o); index_manifest(instance,paths,c);
                require(WritePrivateProfileStringW(L"auction",L"price_keys",L"1",(instance+L"\\services.ini").c_str()),"Save market price schema marker");
                std::string ids;
                for(const auto &v:c.items) if(v.second.reason.empty()) ids+=std::to_string(v.first)+"\t"+v.second.signature+"\n";
                replace_file(definitions,ids);
            }
            require(WritePrivateProfileStringW(L"auction",L"previous_index",b.folder.c_str(),(instance+L"\\services.ini").c_str()),"Save previous market index location");
            require(WritePrivateProfileStringW(L"auction",L"price_keys",L"1",(instance+L"\\services.ini").c_str()),"Save restored market price schema marker");
        } else {
            b=backup(exe,instance,scope);
            std::wstring statement;
            if(operation==CleanInvalid) {
                for(const auto &v:check.conflicts) statement+=L"DELETE FROM "+v.db+L".auction_main WHERE auction_id="+decode(v.order)+L";\n";
            } else for(const auto &db:restore_scope) {
                if(operation==ClearOrders) statement+=L"DELETE FROM "+db+L".auction_main;\nDELETE FROM "+db+L".auction_system_main;\n";
                else if(operation==ClearPrices) for(auto t:{L"auction_average_price",L"auction_manual_average_price",L"auction_roi_average_price"}) statement+=L"DELETE FROM "+db+L"."+t+L";\n";
                else if(operation==Reset) {
                    auto seed=b.folder+L"\\seed.sql"; if(!present(seed)) ResourceZip(assets+L"\\native.zip").extract({{"seed.sql",seed}},false);
                    auto all=read_file(seed); auto marker="USE `"+std::string(db.begin(),db.end())+"`;";
                    auto a=all.find(marker),end=all.find("CREATE DATABASE",a);
                    if(a==std::string::npos || end==std::string::npos) throw std::runtime_error("Market database is absent from packaged seed");
                    auto initial=all.substr(a,end-a); auto last=initial.rfind("-- Current Database:"); if(last!=std::string::npos) initial.resize(last);
                    // Drop only selected database tables, including monthly history,
                    // then replay its exact original seed schema/data.
                    std::string reset="SET FOREIGN_KEY_CHECKS=0;\nUSE `"+std::string(db.begin(),db.end())+"`;\n";
                    for(const auto &t:tables(exe,instance,db)) reset+="DROP TABLE `"+std::string(t.begin(),t.end())+"`;\n";
                    reset+=initial+"\nSET FOREIGN_KEY_CHECKS=1;\n";
                    auto file=b.folder+L"\\reset-"+db+L".sql"; write_private(file,reset); changed=true;
                    if(run_tool(exe,instance,{L"mysql",instance,L"--defaults-file=client.private.cnf"},180000,file).code) throw std::runtime_error("Market seed reset failed");
                } else throw std::runtime_error("Unknown market maintenance action");
            }
            if(!statement.empty()) { changed=true; query(exe,instance,statement); }
            if(operation==Reset) {
                price_schema(exe,instance,scope); monthly_prepare(exe,instance,true);
                if(scope!=1) require(WritePrivateProfileStringW(L"auction",L"price_keys_auction",L"1",(instance+L"\\services.ini").c_str()),"Save gold price schema marker");
                if(scope!=0) require(WritePrivateProfileStringW(L"auction",L"price_keys_point",L"1",(instance+L"\\services.ini").c_str()),"Save cera price schema marker");
            }
            for(const auto &db:restore_scope) {
                if(operation==ClearOrders && query(exe,instance,L"SELECT (SELECT COUNT(*) FROM "+db+L".auction_main)+(SELECT COUNT(*) FROM "+db+L".auction_system_main);")!="0\n")
                    throw std::runtime_error("Auction order deletion verification failed");
                if(operation==ClearPrices && query(exe,instance,L"SELECT (SELECT COUNT(*) FROM "+db+L".auction_average_price)+(SELECT COUNT(*) FROM "+db+L".auction_manual_average_price)+(SELECT COUNT(*) FROM "+db+L".auction_roi_average_price);")!="0\n")
                    throw std::runtime_error("Auction price deletion verification failed");
            }
            if(operation==CleanInvalid && !inspect(exe,instance,c,paths,o,scope).conflicts.empty()) throw std::runtime_error("Incompatible order cleanup verification failed");
        }
        if(file_sha256(pvf)!=c.pvf_hash) throw std::runtime_error("PVF changed during maintenance; changes will be rolled back");
    } catch(...) {
        auto failure=std::current_exception();
        if(changed) {
            if(!b.dumps.empty()) rollback(exe,instance,b);
            for(unsigned i=0;i<paths.size();++i) replace_file(paths[i],original[i]);
            replace_file(instance+L"\\services.ini",config);
            if(!old_manifest.empty()) replace_file(index_ini,old_manifest); else if(present(index_ini)) require(DeleteFileW(index_ini.c_str()),"Remove rolled-back market index marker");
            if(!old_js.empty()) replace_file(dp2_path,old_js);
            if(!old_definitions.empty()) replace_file(definitions,old_definitions); else if(present(definitions)) require(DeleteFileW(definitions.c_str()),"Roll back market definition marker");
        }
        // Retain stopped services on failure: an incompatible PVF must never
        // re-enter startup merely because the maintenance itself was rolled back.
        std::rethrow_exception(failure);
    }
    auto fresh=load_services(instance,services.empty()?L"":services[0].host.substr(0,services[0].host.find_last_of(L"\\/")));
    for(auto &s:services) for(const auto &f:fresh) if(s.name==f.name) s.environment=f.environment;
    for(auto i:restart) start_service(services[i],instance,job);
    return inspect(exe,instance,c,paths,options(instance),scope).report+L"\r\n维护完成；原运行服务已重新拉起（业务状态查看原始日志）。\r\n备份："+b.folder;
}
} // namespace market
