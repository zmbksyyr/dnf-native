// Shared launcher/game key preparation; independent of optional modules.
#pragma once
#include "../login/crypto.h"
#include <wincrypt.h>

static std::string default_game_private_key() {
    HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(201),RT_RCDATA);
    require(resource!=nullptr,"Find default game key");
    HGLOBAL loaded=LoadResource(nullptr,resource);
    require(loaded!=nullptr,"Load default game key");
    const char *bytes=static_cast<const char *>(LockResource(loaded));
    require(bytes!=nullptr,"Read default game key");
    return {bytes,SizeofResource(nullptr,resource)};
}

static std::string game_public_key(const std::string &key) {
    char result[1024];
    if(rsa_public_key_pem(key.data(),key.size(),result,sizeof(result)))
        throw std::runtime_error("Invalid launcher RSA private key (PKCS#1 required)");
    return result;
}

static std::string key_der(const std::string &pem) {
    DWORD size=0;
    require(CryptStringToBinaryA(pem.data(),static_cast<DWORD>(pem.size()),CRYPT_STRING_BASE64HEADER,nullptr,&size,nullptr,nullptr),"Decode RSA key");
    std::string bytes(size,0);
    require(CryptStringToBinaryA(pem.data(),static_cast<DWORD>(pem.size()),CRYPT_STRING_BASE64HEADER,reinterpret_cast<BYTE *>(bytes.data()),&size,nullptr,nullptr),"Decode RSA key");
    bytes.resize(size);return bytes;
}

static std::wstring game_directory(const std::wstring &instance) {
    auto manifest=instance+L"\\services.ini";
    auto binary=instance+L"\\"+setting(manifest,L"game",L"root")+setting(manifest,L"game",L"elf");
    if(setting(manifest,L"game",L"elf").empty()) throw std::runtime_error("Game service is missing");
    return binary.substr(0,binary.find_last_of(L"\\/"));
}

static void prepare_game_keys(const std::wstring &instance) {
    auto folder=game_directory(instance),priv=folder+L"\\privatekey.pem",pub=folder+L"\\publickey.pem";
    bool existing=GetFileAttributesW(priv.c_str())!=INVALID_FILE_ATTRIBUTES;
    auto secret=existing?read_file(priv):default_game_private_key();
    auto public_key=game_public_key(secret);
    if(GetFileAttributesW(pub.c_str())!=INVALID_FILE_ATTRIBUTES && key_der(read_file(pub))!=key_der(public_key))
        throw std::runtime_error("Game RSA keys do not match; existing keys preserved");
    if(!existing) write_private(priv,secret);
    if(GetFileAttributesW(pub.c_str())==INVALID_FILE_ATTRIBUTES) write_private(pub,public_key);
}
