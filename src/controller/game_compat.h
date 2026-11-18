// Instance scripts for the pinned game's documented DP2 interface.
#pragma once
#include <sstream>

static const char auction_level_lua[] = R"LUA(-- DNF Native auction compatibility begin
do
    local dp, dpx = _DP, _DPX
    local pc = dpx.reloc(0x08213f27)
    -- Keep the original signed comparison and error path. Only its operand is
    -- configurable; the bundled DPX setter supplies a stack byte instead.
    local function verify()
        assert((dp.mem.read(pc, 0) & 0xffff) == 0xf883 and
               (dp.mem.read(pc, 3) & 0xffffffff) == 0x84c09e0f and
               (dp.mem.read(pc, 6) & 0xffffffff) == 0x2c74c084,
               "auction minimum-level instructions differ")
    end
    verify()
    dpx.set_auction_min_level = function(level)
        assert(math.type(level) == "integer" and level >= 0 and level <= 127,
               "auction minimum-level operand must be an integer in 0..127")
        verify()
        local previous = dp.mem.read(pc, 2) & 255
        assert(dp.mem.hotfix(pc + 2, 1, string.char(level)),
               "auction minimum-level write failed")
        assert((dp.mem.read(pc, 2) & 255) == level,
               "auction minimum-level readback differs")
        io.stderr:write(string.format("auction minimum-level pc=0x%x original=%02x replacement=%02x scope=execution-memory; original strict comparison retained\n", pc + 2, previous, level))
    end
end
-- DNF Native auction compatibility end
)LUA";

static void prepare_gm_online(const Service &service) {
    const auto login = service.root + L"\\login.private.cnf";
    if (GetFileAttributesW(login.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (service.arguments.size()<2) throw std::runtime_error("GM bridge game configuration argument missing");
    std::wstring config_name = service.arguments[1];
    if (config_name.find_first_of(L"\\/:") != std::wstring::npos)
        throw std::runtime_error("GM bridge requires the game's configuration name");
    const auto game_config = service.root + L"\\home\\neople\\game\\cfg\\" + config_name + L".cfg";
    const std::string text = read_file(game_config);
    auto number = [&](const std::string &key) -> unsigned {
        std::istringstream lines(text); std::string line;
        while (std::getline(lines, line)) {
            auto at = line.find('=');
            if (at == std::string::npos) continue;
            std::string name = line.substr(0, at);
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return c==' ' || c=='\t' || c=='\r'; }), name.end());
            if (name != key) continue;
            std::istringstream value(line.substr(at+1)); unsigned result;
            if (value >> result && result > 0 && result < 1000) return result;
            break;
        }
        throw std::runtime_error("GM bridge game channel setting invalid");
    };
    const unsigned channel = number("server_group")*1000+number("channel_no");
    const auto host = setting(login,L"client",L"host"), user = setting(login,L"client",L"user"), password = setting(login,L"client",L"password");
    const unsigned port = GetPrivateProfileIntW(L"client",L"port",3306,login.c_str());
    if (host.empty() || user.empty() || password.empty() || port<1 || port>65535)
        throw std::runtime_error("GM bridge database connection invalid; inspect login.private.cnf");
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(202), RT_RCDATA);
    require(resource != nullptr,"Find GM bridge script");
    HGLOBAL loaded = LoadResource(nullptr,resource);
    require(loaded != nullptr,"Load GM bridge script");
    const char *bytes = static_cast<const char *>(LockResource(loaded));
    require(bytes != nullptr,"Read GM bridge script");
    const auto helper = service.root + L"\\dp2\\dnf-gm-online.lua";
    const auto config = service.root + L"\\dp2\\gm-online.private.json";
    const std::string data(bytes,SizeofResource(nullptr,resource));
    if (GetFileAttributesW(helper.c_str())!=INVALID_FILE_ATTRIBUTES && read_file(helper)!=data) {
        auto backup = helper + L".before-native-update";
        if (GetFileAttributesW(backup.c_str())==INVALID_FILE_ATTRIBUTES)
            require(CopyFileW(helper.c_str(),backup.c_str(),TRUE),"Back up GM bridge script");
    }
    write_private(helper,data,CREATE_ALWAYS);
    const std::string marker = "\"generated_by\":\"DNF Native GM bridge v1\"";
    const std::string connection = "{"+marker+",\"host\":\""+json_escape(utf8_path(host))+"\",\"port\":"+std::to_string(port)+
                      ",\"user\":\""+json_escape(utf8_path(user))+"\",\"password\":\""+json_escape(utf8_path(password))+
                      "\",\"channel\":"+std::to_string(channel)+"}\n";
    if (GetFileAttributesW(config.c_str())==INVALID_FILE_ATTRIBUTES) write_private(config,connection);
    else {
        const auto previous = read_file(config);
        if (previous.find(marker)!=std::string::npos && previous!=connection) {
            const auto backup = config + L".before-native-update";
            if (GetFileAttributesW(backup.c_str())==INVALID_FILE_ATTRIBUTES)
                require(CopyFileW(config.c_str(),backup.c_str(),TRUE),"Back up GM bridge configuration");
            write_private(config,connection,CREATE_ALWAYS);
        }
    }
    const auto entry = service.root + L"\\dp2\\df_game_r.lua";
    auto source = read_file(entry);
    const std::string begin = "-- DNF Native GM bridge begin\n", end = "-- DNF Native GM bridge end\n";
    const std::string segment = begin + "do\n local ok,err=pcall(dofile,'/dp2/dnf-gm-online.lua')\n if not ok then io.stderr:write('GM bridge unavailable: ',tostring(err),'\\n') end\nend\n" + end;
    auto at = source.find(begin);
    if (at != std::string::npos) {
        auto last = source.find(end,at);
        if (last==std::string::npos) throw std::runtime_error("Incomplete GM bridge entry; preserve instance for diagnosis");
        if (source.substr(at,last+end.size()-at)==segment) return;
        source.replace(at,last+end.size()-at,segment);
    } else source += "\n" + segment;
    const auto backup = entry + L".before-native-gm";
    if (GetFileAttributesW(backup.c_str())==INVALID_FILE_ATTRIBUTES)
        require(CopyFileW(entry.c_str(),backup.c_str(),TRUE),"Back up GM bridge entry");
    const auto temporary = entry + L".native-gm-tmp";
    write_private(temporary,source);
    require(MoveFileExW(temporary.c_str(),entry.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),"Install GM bridge entry");
}

static void prepare_game_compat(const Service &service) {
    if (service.name != L"game" ||
        std::find(service.environment.begin(), service.environment.end(), dp2_preload) == service.environment.end()) return;
    if (service.elf_hash != L"da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979")
        throw std::runtime_error("DP2 auction compatibility requires the pinned game ELF");
    prepare_gm_online(service);
    const auto file = service.root + L"\\dp2\\df_game_r.lua";
    std::string source = read_file(file);
    const std::string begin = "-- DNF Native auction compatibility begin\n";
    const std::string end = "-- DNF Native auction compatibility end\n";
    if (source.rfind(auction_level_lua, 0) == 0) return;
    if (source.rfind(begin, 0) == 0) {
        size_t at = source.find(end);
        if (at == std::string::npos) throw std::runtime_error("Incomplete game compatibility script; preserve the instance for diagnosis");
        source.erase(0, at + end.size());
    }
    // Preserve the exact user entry before the first installation. Restarting
    // the ELF restores its original code; this file restores the Lua entry.
    auto backup = file + L".before-native-auction-compat";
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES)
        require(CopyFileW(file.c_str(), backup.c_str(), TRUE), "Back up game Lua entry");
    auto temporary = file + L".native-auction-tmp";
    write_private(temporary, std::string(auction_level_lua) + source);
    require(MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Install game auction compatibility");
}
