#include "mod_runtime.h"
#include "crc32.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <chrono>
namespace fs=std::filesystem;
static void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
static void activate() {}
static std::vector<std::vector<uint8_t>> approved;
static int compiled(const uint8_t* p,uint32_t n) {
    for(const auto& image:approved)if(image.size()==n && !std::memcmp(image.data(),p,n))return 1;
    return 0;
}
static void write(const fs::path& path,const std::vector<uint8_t>& data) {
    std::ofstream out(path,std::ios::binary);out.write((const char*)data.data(),data.size());
}
static std::string read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
static std::string crc(const std::vector<uint8_t>& image) {
    char text[9];std::snprintf(text,sizeof text,"%08x",crc32_compute(image.data()+16,image.size()-16));return text;
}
int main() {
    try {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path root=fs::absolute("cartridge-patch-fixtures-"+std::to_string(stamp));
        const auto package=root/"packages/program.test/1.0.0",art=root/"packages/art.test/1.0.0";
        fs::create_directories(package);fs::create_directories(art/"pack");
        std::vector<uint8_t> stock(16400,0);std::memcpy(stock.data(),"NES\x1a",4);stock[4]=1;
        write(root/"stock.nes",stock);
        auto base=stock;base[7]=8;base[11]=9;base[16]=0xa9;auto combo=base;combo[16]=0xe8;
        approved={base,combo};
        const std::vector<uint8_t> base_ips={'P','A','T','C','H',0,0,7,0,1,8,0,0,11,0,1,9,0,0,16,0,1,0xa9,'E','O','F'};
        const std::vector<uint8_t> addon_ips={'P','A','T','C','H',0,0,16,0,1,0xe8,'E','O','F'};
        write(package/"base.ips",base_ips);write(package/"addon.ips",addon_ips);
        auto manifest=[&](const std::string& path="base.ips",int order=10) {
            std::ofstream f(package/"manifest.toml");
            f<<"format_version=1\nid=\"program.test\"\nversion=\"1.0.0\"\nname=\"Program\"\n"
              "[[target]]\ngame_id=\"test\"\nrom_crc32=\""<<crc(stock)<<"\"\n"
              "[[feature]]\nid=\"base\"\nname=\"Base\"\ndefault_enabled=false\nexclusive_groups=[\"stock-renderer\"]\n"
              "[[feature]]\nid=\"addon\"\nname=\"Addon\"\ndefault_enabled=false\nrequires=[\"program.test/base\"]\n"
              "[[feature]]\nid=\"renderer\"\nname=\"Renderer\"\ndefault_enabled=true\nexclusive_group=\"stock-renderer\"\n"
              "[[plugin]]\nfeature=\"base\"\nid=\"program.base\"\n"
              "[[plugin]]\nfeature=\"addon\"\nid=\"program.addon\"\n"
              "[[plugin]]\nfeature=\"renderer\"\nid=\"program.renderer\"\n"
              "[[cartridge_patch]]\nfeature=\"base\"\npath=\""<<path<<"\"\norder=0\nhd_graphics=[\"art.test\"]\n"
              "[[cartridge_patch]]\nfeature=\"addon\"\npath=\"addon.ips\"\norder="<<order<<"\n";
        };
        manifest();
        std::ofstream(art/"pack/hires.txt")<<"<ver>106\n<scale>2\n";
        write(art/"old.ips",{'P','A','T','C','H',0,0,20,0,1,0x55,'E','O','F'});
        auto legacy=stock;legacy[20]=0x55;
        std::ofstream(art/"manifest.toml")<<"format_version=1\nid=\"art.test\"\nversion=\"1.0.0\"\nname=\"Art\"\n"
          "[[target]]\ngame_id=\"test\"\nrom_crc32=\""<<crc(stock)<<"\"\n"
          "[[feature]]\nid=\"art\"\nname=\"Art\"\ndefault_enabled=false\n"
          "[[hd_pack]]\nfeature=\"art\"\ndirectory=\"pack\"\npatch=\"old.ips\"\npatched_rom_crc32=\""<<crc(legacy)<<"\"\n";
        check(nes_mod_register_activation_plugin("program.base",activate),"base plugin");
        check(nes_mod_register_activation_plugin("program.addon",activate),"addon plugin");
        check(nes_mod_register_activation_plugin("program.renderer",activate),"renderer plugin");
        check(nes_mod_register_activation_plugin("nesrecomp.hd-pack",activate),"graphics plugin");
        auto init=[&]() {std::string error;return NESRecomp::mod_runtime_initialize(root,"test",crc(stock),&error);};
        std::string error;check(init(),"initialize");
        const auto* p=NESRecomp::mod_runtime_launcher_provider();
        auto enable=[&](const char* id,const char* feature,int value=1) {return p->feature_enable(p->ctx,id,feature,value);};
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"stock commit");uint32_t size=99;
        check(!nes_mod_cartridge_image(&size) && !size && !nes_mod_cartridge_save_namespace(),"default changed stock");
        nes_mod_lock_cartridge_image();check(!enable("program.test","addon"),"live stock program changed");
        check(init() && enable("program.test","addon"),"dependency selection");
        check(!nes_mod_feature_selected("program.test","renderer"),"selected incompatible renderer still reported active");
        check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"missing native authority accepted");
        check(nes_mod_register_hd_image_validator(compiled),"native validator");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        check(nes_mod_cartridge_image(&size) && size==combo.size() && !std::memcmp(nes_mod_cartridge_image(nullptr),combo.data(),size),"ordered composition");
        check(!std::strcmp(nes_mod_cartridge_save_namespace(),"program.test"),"separate progress namespace");
        check(enable("art.test","art") && NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        check(nes_mod_hd_pack() && nes_mod_hd_pack()->graphics_only && !nes_mod_hd_pack()->patched_image && !nes_mod_hd_pack()->patched_payload,"old HD code overlaid selected program");
        check(!std::memcmp(nes_mod_cartridge_image(nullptr),combo.data(),size),"graphics changed cartridge");
        const auto original=read(root/"stock.nes"),selection=read(root/"state.toml");
        nes_mod_lock_cartridge_image();nes_mod_lock_hd_pack();
        check(nes_mod_feature_requires_restart("program.test","base") && nes_mod_feature_requires_restart("program.test","addon"),"boot selection unlocked");
        check(nes_mod_feature_requires_restart("program.test","renderer") && !enable("program.test","renderer"),"live renderer disabled the locked cartridge program");
        check(!enable("program.test","addon",0),"live companion toggle");
        check(!nes_mod_runtime_commit_netplay_c((root/"stock.nes").string().c_str()) && !p->commit_netplay(p->ctx,(root/"stock.nes").string().c_str()),"live patched online commit");
        check(!std::memcmp(nes_mod_cartridge_image(nullptr),combo.data(),size),"rejection changed running image");
        check(init(),"fresh launch");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"fresh program commit");
        check(nes_mod_runtime_commit_netplay_c((root/"stock.nes").string().c_str()) && !nes_mod_cartridge_image(nullptr) && !nes_mod_hd_pack() && !nes_mod_cartridge_save_namespace(),"online did not clear offline images");
        check(read(root/"state.toml")==selection,"online changed offline choices");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"offline restoration");
        auto bad=addon_ips;bad[10]=0xea;write(package/"addon.ips",bad);
        check(init() && !NESRecomp::mod_runtime_commit(root/"stock.nes",&error) && !nes_mod_cartridge_image(nullptr),"unknown program exposed");
        check(read(root/"stock.nes")==original && read(root/"state.toml")==selection,"failed patch modified owner data");
        write(package/"addon.ips",addon_ips);manifest("base.ips",0);
        check(init() && !NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"ambiguous patch order");
        manifest("../base.ips");check(!init(),"unsafe package path");
        std::cout<<"PASS: optional/dependent patches, native authority, ordering, geometry, progress namespace, HD graphics, live locks and online/offline controls\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
