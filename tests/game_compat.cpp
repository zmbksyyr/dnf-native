// Private installer boundary test; executes the production header/file APIs.
#include "../src/win32_files.h"
#include <algorithm>
struct Service {
    std::wstring name, root, elf_hash;
    std::vector<std::wstring> environment;
};
static const std::wstring dp2_preload=L"LD_PRELOAD=/dp2/libdp2pre.so";
#include "../src/controller/game_compat.h"

int wmain(int argc,wchar_t **argv) try {
    require(argc==2,"Pass private test directory");
    std::wstring root=argv[1]; directory(root+L"\\dp2");
    const auto file=root+L"\\dp2\\df_game_r.lua";
    const auto backup=file+L".before-native-auction-compat";
    Service service{L"game",root,L"da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979",{}};
    prepare_game_compat(service); // Disabled DP2 must not even open the entry.
    require(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES,"Disabled DP2 changed entry");
    service.environment={dp2_preload}; service.name=L"auction";
    prepare_game_compat(service);
    service.name=L"game";
    const std::string custom="-- custom user entry\r\nprint('preserve me')\r\n";
    write_private(file,custom);
    auto valid_hash=service.elf_hash; service.elf_hash=L"unknown";
    bool rejected=false; try { prepare_game_compat(service); } catch(const std::exception &) { rejected=true; }
    require(rejected && read_file(file)==custom,"Unknown ELF changed entry");
    service.elf_hash=valid_hash;
    prepare_game_compat(service);
    const auto installed=read_file(file);
    require(installed==std::string(auction_level_lua)+custom && read_file(backup)==custom,"Install/backup differs");
    prepare_game_compat(service);
    require(read_file(file)==installed && read_file(backup)==custom,"Repeated install changed entry/backup");
    const std::string later="-- independent later user change\n";
    write_private(file,installed+later,CREATE_ALWAYS); prepare_game_compat(service);
    require(read_file(file)==installed+later && read_file(backup)==custom,"Later custom content changed");
    write_private(file,"-- DNF Native auction compatibility begin\n-- old native block\n-- DNF Native auction compatibility end\n"+custom+later,CREATE_ALWAYS);
    prepare_game_compat(service);
    require(read_file(file)==installed+later && read_file(backup)==custom,"Native block update lost user content");
    const std::string incomplete="-- DNF Native auction compatibility begin\n-- incomplete\n";
    write_private(file,incomplete,CREATE_ALWAYS); rejected=false;
    try { prepare_game_compat(service); } catch(const std::exception &) { rejected=true; }
    require(rejected && read_file(file)==incomplete && read_file(backup)==custom,"Incomplete block was overwritten");
    write_private(root+L"\\production-compat.lua",auction_level_lua);
    puts("GAME_COMPAT_INSTALL PASS backup, idempotence, later custom content, block update, disabled DP2, unknown ELF, incomplete block");
    return 0;
} catch(const std::exception &e) { fprintf(stderr,"GAME_COMPAT_INSTALL FAIL %s\n",e.what()); return 1; }
