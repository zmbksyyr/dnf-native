// Independent PVF reader. Category evidence comes from this PVF's own iteminfo
// and definition types; ambiguous categories are reported, never invented.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>

namespace market {
inline uint32_t word(const std::string &s,size_t at) {
    if(at>s.size() || s.size()-at<4) throw std::runtime_error("PVF field exceeds file");
    uint32_t n; std::memcpy(&n,s.data()+at,4); return n;
}
inline std::string normalized(std::string s) {
    for(char &c:s) { if(c=='\\') c='/'; if(c>='A' && c<='Z') c+=32; }
    return s;
}
inline std::wstring wide(const std::string &s,UINT cp) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if(!n) throw std::runtime_error("PVF text cannot be decoded without loss");
    std::wstring out(n,0); MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),out.data(),n); return out;
}
inline std::string utf8(const std::wstring &s) {
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    if(!n && !s.empty()) throw std::runtime_error("PVF Unicode conversion failed");
    std::string out(n,0); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr); return out;
}
struct Token { unsigned type; int32_t value; };
class Pvf {
    struct Entry { uint32_t size,crc,offset; };
    std::string data,strings;
    size_t base=0;
    std::map<std::string,Entry> entries;
    std::map<int,std::map<std::string,std::string>> links;
    std::map<int,std::string> link_files;
    static std::string decrypt(std::string s,uint32_t crc) {
        if(s.size()%4) throw std::runtime_error("PVF encrypted span is not word aligned");
        for(size_t i=0;i<s.size();i+=4) { uint32_t n=word(s,i)^(crc^0x81a79011u); n=(n>>6)|(n<<26); std::memcpy(s.data()+i,&n,4); }
        return s;
    }
    UINT detect_encoding() const {
        const wchar_t common[]=L"的一是不在人有我和这這为為个個中来來上大们們到说說时時要会會对對能以地得可也就出子生自年过過于於着著下";
        long long score[2]={}; UINT cps[]={936,950}; unsigned count=word(strings,0);
        for(unsigned i=0;i<count;i+=std::max(1u,count/16384)) {
            auto s=raw_string((int)i);
            if(std::none_of(s.begin(),s.end(),[](unsigned char c){return c>=128;})) continue;
            for(unsigned j=0;j<2;++j) try {
                for(wchar_t c:wide(s,cps[j])) { if(c && std::wcschr(common,c)) score[j]+=8; if(c>=0xe000 && c<=0xf8ff) score[j]-=4; }
            } catch(...) { score[j]-=(long long)s.size(); }
        }
        if(score[0]>0 && score[0]-score[1]>=16) return 936;
        if(score[1]>0 && score[1]-score[0]>=16) return 950;
        throw std::runtime_error("PVF text encoding is ambiguous; no index was generated");
    }
public:
    UINT encoding=0;
    explicit Pvf(const std::wstring &path) : data(read_file(path)) {
        uint32_t guid=word(data,0); size_t head=4ull+guid;
        uint32_t length=word(data,head+4),crc=word(data,head+8),count=word(data,head+12);
        base=head+16ull+length;
        if(base>data.size()) throw std::runtime_error("PVF directory is truncated");
        auto tree=decrypt(data.substr(head+16,length),crc); size_t at=0;
        for(uint32_t i=0;i<count;++i) {
            uint32_t n=word(tree,at+4); at+=8;
            if(n>tree.size()-at) throw std::runtime_error("PVF pathname exceeds directory");
            auto name=normalized(tree.substr(at,n)); at+=n;
            Entry e{word(tree,at),word(tree,at+4),word(tree,at+8)}; at+=12;
            if(e.offset>data.size()-base || ((uint64_t(e.size)+3)&~3ull)>data.size()-base-e.offset)
                throw std::runtime_error("PVF entry exceeds file");
            if(!entries.emplace(name,e).second) throw std::runtime_error("PVF contains duplicate normalized path");
        }
        strings=file("stringtable.bin"); uint32_t n=word(strings,0);
        if((uint64_t(n)+2)*4>strings.size()) throw std::runtime_error("PVF string directory is invalid");
        for(uint32_t i=0;i<=n;++i) {
            uint32_t off=word(strings,4ull+i*4);
            if(off<4ull*(n+1) || off>strings.size()-4 || (i && off<word(strings,i*4)))
                throw std::runtime_error("PVF string offsets are invalid");
        }
        encoding=detect_encoding();
        if(has("n_string.lst")) {
            auto t=tokens(file("n_string.lst"));
            for(size_t i=0;i+1<t.size();i+=2) {
                if(t[i].type!=2 || t[i+1].type!=7) throw std::runtime_error("PVF name-link list has unknown grammar");
                link_files[t[i].value]=normalized(raw_string(t[i+1].value));
            }
        }
    }
    bool has(const std::string &s) const { return entries.count(normalized(s))!=0; }
    std::string identity(const std::string &s) const {
        auto e=entries.at(normalized(s)); return normalized(s)+":"+std::to_string(e.size)+":"+std::to_string(e.crc);
    }
    std::string file(const std::string &s) const {
        auto i=entries.find(normalized(s)); if(i==entries.end()) throw std::runtime_error("PVF missing file: "+s);
        auto e=i->second; auto out=decrypt(data.substr(base+e.offset,(uint64_t(e.size)+3)&~3ull),e.crc); out.resize(e.size); return out;
    }
    std::string raw_string(int i) const {
        if(i<0 || unsigned(i)>=word(strings,0)) throw std::runtime_error("PVF string reference is invalid");
        uint32_t a=word(strings,4ull+i*4),b=word(strings,8ull+i*4);
        auto s=strings.substr(a+4,b-a); while(!s.empty() && !s.back()) s.pop_back(); return s;
    }
    std::string text(int i) const { return utf8(wide(raw_string(i),encoding)); }
    static std::vector<Token> tokens(const std::string &s) {
        if(s.size()<2 || (s.size()-2)%5) throw std::runtime_error("PVF script token stream is invalid");
        std::vector<Token> t; t.reserve((s.size()-2)/5);
        for(size_t i=2;i<s.size();i+=5) t.push_back({(unsigned char)s[i],(int32_t)word(s,i+1)});
        return t;
    }
    std::string value(const std::vector<Token> &t,size_t &i,bool optional=false) {
        if(t[i].type==9) {
            int link=t[i].value;
            if(++i==t.size() || t[i].type!=10 || !link_files.count(link)) throw std::runtime_error("PVF indirect name reference is invalid");
            if(!links.count(link)) {
                std::map<std::string,std::string> records;
                std::istringstream lines(file(link_files.at(link))); std::string line;
                while(std::getline(lines,line)) {
                    if(!line.empty() && line.back()=='\r') line.pop_back(); size_t p=line.find('>');
                    if(p!=std::string::npos) records[line.substr(0,p)]=line.substr(p+1);
                }
                links.emplace(link,std::move(records));
            }
            auto key=raw_string(t[i].value); auto &records=links.at(link); auto p=records.find(key);
            if(p==records.end()) { if(optional) return {}; throw std::runtime_error("PVF localized name is missing: "+key); }
            return utf8(wide(p->second,encoding));
        }
        if(t[i].type>=5 && t[i].type<=8) return text(t[i].value);
        if(t[i].type==2) return std::to_string(t[i].value);
        throw std::runtime_error("PVF metadata has an unsupported value type");
    }
};
using Fields=std::array<std::string,17>;
struct Definition { Fields fields{}; std::string signature,reason; };
struct Catalog {
    std::map<int,Definition> items;
    unsigned max_level=1, max_refine=7, excluded=0;
    UINT encoding=0;
    std::string pvf_hash;
    std::string dat() const {
        std::string s;
        for(const auto &v:items) if(v.second.reason.empty()) {
            const auto &f=v.second.fields;
            for(unsigned i=0;i<17;++i) { if(i) s+=' '; s+=(i==14 || i==15) ? "`"+f[i]+"`" : f[i]; }
            s+="\r\n";
        }
        return s;
    }
};
inline std::map<int,Fields> parse_dat(const std::string &text) {
    std::map<int,Fields> rows; std::istringstream lines(text); std::string line;
    while(std::getline(lines,line)) {
        if(line.empty() || line=="\r") continue;
        Fields fields; size_t pos=0;
        for(unsigned i=0;i<17;++i) {
            pos=line.find_first_not_of(" \t\r",pos);
            if(pos==std::string::npos) throw std::runtime_error("iteminfo has incomplete row");
            bool quoted=line[pos]=='`'; if(quoted) ++pos;
            size_t end=quoted ? line.find('`',pos) : line.find_first_of(" \t\r",pos);
            if(end==std::string::npos) end=line.size();
            fields[i]=line.substr(pos,end-pos); pos=end+(quoted ? 1:0);
        }
        int id=std::stoi(fields[0]);
        if(!rows.emplace(id,fields).second) throw std::runtime_error("iteminfo has duplicate ID");
    }
    return rows;
}
inline Catalog catalog(const std::wstring &path) {
    Catalog out; out.pvf_hash=file_sha256(path); Pvf p(path); out.encoding=p.encoding;
    std::map<int,Fields> raw;
    if(p.has("etc/iteminfo.dat")) {
        auto t=Pvf::tokens(p.file("etc/iteminfo.dat")); size_t i=0;
        while(i<t.size()) {
            // The packaged Big5 PVF ends this table with an [explain] string.
            // It describes the file, rather than adding another iteminfo row.
            if(t[i].type==5 && p.text(t[i].value)=="[explain]") {
                if(++i==t.size()) throw std::runtime_error("PVF iteminfo explanation is incomplete");
                p.value(t,i); if(++i!=t.size()) throw std::runtime_error("PVF iteminfo has unknown trailing metadata");
                break;
            }
            Fields f;
            for(unsigned k=0;k<17;++k) { if(i>=t.size()) throw std::runtime_error("PVF iteminfo row is incomplete"); f[k]=p.value(t,i); ++i; }
            raw[std::stoi(f[0])]=f;
        }
    }
    std::map<std::string,std::set<std::string>> categories;
    std::map<int,std::vector<std::string>> category_keys;
    for(const std::string family:{"equipment","stackable"}) {
        auto list=Pvf::tokens(p.file(family+"/"+family+".lst"));
        for(size_t i=0;i+1<list.size();i+=2) {
            if(list[i].type!=2 || list[i+1].type!=7) throw std::runtime_error("PVF item list has unknown grammar");
            int id=list[i].value; Definition d; d.fields.fill("0"); d.fields[0]=std::to_string(id);
            std::string name=normalized(p.raw_string(list[i+1].value)); if(!p.has(name)) name=family+"/"+name;
            try {
                auto t=Pvf::tokens(p.file(name)); std::map<std::string,std::vector<std::string>> attrs;
                for(size_t j=0;j<t.size();++j) if(t[j].type==5) {
                    auto label=p.text(t[j].value);
                    if(label!="[name]" && label!="[name2]" && label!="[rarity]" && label!="[minimum level]" && label!="[equipment type]" && label!="[stackable type]" && label!="[sub type]" && label!="[usable job]") continue;
                    auto &values=attrs[label];
                    while(j+1<t.size() && t[j+1].type!=5) { ++j; values.push_back(p.value(t,j,label=="[name2]")); }
                }
                auto scalar=[&](const std::string &s,const std::string &fallback) { auto a=attrs.find(s); return a==attrs.end() || a->second.empty() ? fallback : a->second[0]; };
                if(raw.count(id)) d.fields=raw.at(id);
                d.fields[0]=std::to_string(id); d.fields[1]=scalar("[rarity]","0");
                d.fields[13]=scalar("[minimum level]","0"); d.fields[14]=scalar("[name]",""); d.fields[15]=scalar("[name2]","");
                if(d.fields[14].empty()) throw std::runtime_error("definition has no name");
                for(unsigned k:{14u,15u}) if(d.fields[k].size()>250 || d.fields[k].find_first_of("`\r\n\0",0,4)!=std::string::npos)
                    throw std::runtime_error("name cannot be represented in original iteminfo grammar/buffer");
                unsigned level=(unsigned)std::stoul(d.fields[13]); if(level>127) throw std::runtime_error("item level exceeds original search representation");
                out.max_level=std::max(out.max_level,level);
                if(!raw.count(id)) {
                    for(unsigned k=2;k<=12;++k) d.fields[k]="1";
                    auto jobs=attrs.find("[usable job]");
                    static const char *job_names[]={"[swordman]","[fighter]","[gunner]","[mage]","[priest]","[at gunner]","[thief]","[at fighter]","[at mage]","[demonic swordman]","[creator]"};
                    if(jobs!=attrs.end() && std::find(jobs->second.begin(),jobs->second.end(),"[all]")==jobs->second.end()) {
                        for(unsigned k=2;k<=12;++k) d.fields[k]="0";
                        for(auto job:jobs->second) {
                            while(!job.empty() && job.back()==' ') job.pop_back();
                            if(job=="[creator mage]") job="[creator]";
                            auto q=std::find(std::begin(job_names),std::end(job_names),job);
                            if(q==std::end(job_names)) throw std::runtime_error("unknown usable job: "+job);
                            d.fields[2+(q-std::begin(job_names))]="1";
                        }
                    }
                }
                auto type=family+":"+scalar("[equipment type]",scalar("[stackable type]",""))+":"+scalar("[sub type]","0");
                auto directory=name.substr(0,name.find_last_of('/'));
                auto &keys=category_keys[id];
                while(!directory.empty()) {
                    keys.push_back(type+":"+directory);
                    auto slash=directory.find_last_of('/'); if(slash==std::string::npos) break; directory.resize(slash);
                }
                d.signature=type+":"+p.identity(name);
                if(raw.count(id) && std::stoul(d.fields[16])>0 && std::stoul(d.fields[16])<=65535) {
                    for(const auto &key:keys) categories[key].insert(d.fields[16]);
                } else d.fields[16]="0";
            } catch(const std::exception &e) { d.reason=e.what(); }
            if(!out.items.emplace(id,std::move(d)).second) throw std::runtime_error("PVF duplicates an item ID between definitions");
        }
    }
    for(auto &entry:out.items) {
        auto &d=entry.second;
        if(d.reason.empty() && d.fields[16]=="0") {
            for(const auto &key:category_keys[entry.first]) {
                auto c=categories.find(key);
                if(c!=categories.end()) { if(c->second.size()==1) d.fields[16]=*c->second.begin(); break; }
            }
            if(d.fields[16]=="0") d.reason="category lacks unambiguous evidence in current PVF";
        }
        if(!d.reason.empty()) ++out.excluded;
    }
    if(p.has("etc/upgrade_separate.etc")) {
        auto t=Pvf::tokens(p.file("etc/upgrade_separate.etc"));
        for(size_t i=0;i+1<t.size();++i) if(t[i].type==5 && p.text(t[i].value)=="[separate upgrade max]") {
            if(t[i+1].type!=2 || t[i+1].value<0 || t[i+1].value>31) throw std::runtime_error("PVF refine maximum exceeds original five-bit item protocol");
            out.max_refine=(unsigned)t[i+1].value;
        }
    }
    if(file_sha256(path)!=out.pvf_hash) throw std::runtime_error("PVF changed during index generation; retry after replacing it");
    return out;
}
} // namespace market
