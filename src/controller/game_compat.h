// Instance scripts for the pinned game's documented DP2 interface.
#pragma once

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

static void prepare_game_compat(const Service &service) {
    if (service.name != L"game" ||
        std::find(service.environment.begin(), service.environment.end(), dp2_preload) == service.environment.end()) return;
    if (service.elf_hash != L"da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979")
        throw std::runtime_error("DP2 auction compatibility requires the pinned game ELF");
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
