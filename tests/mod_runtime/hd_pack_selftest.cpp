#include "mod_runtime.h"
#include "crc32.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>

namespace fs=std::filesystem;
static void check(bool v,const char *message) { if(!v)throw std::runtime_error(message); }
static void activate() {}
static void write(const fs::path& p,const std::vector<uint8_t>& bytes) {
    std::ofstream f(p,std::ios::binary);f.write((const char*)bytes.data(),bytes.size());
}
static std::string crc(const std::vector<uint8_t>& image) {
    char text[9];std::snprintf(text,sizeof text,"%08x",crc32_compute(image.data()+16,image.size()-16));return text;
}
int main(int argc,char **argv) {
    /* Integration driver for an owner-supplied pack produced by the shared
     * importer. Install through the actual launcher provider, then enable it. */
    if(argc==6) {
        try {
            std::ifstream input(argv[4],std::ios::binary);
            std::vector<uint8_t> image((std::istreambuf_iterator<char>(input)),{});
            check(image.size()>=16,"integration ROM");
            check(nes_mod_register_activation_plugin("nesrecomp.hd-pack",activate),"registration");
            std::string error;
            check(NESRecomp::mod_runtime_initialize(argv[2],argv[3],crc(image),&error),error.c_str());
            const auto *p=NESRecomp::mod_runtime_launcher_provider();
            check(p->install_archive(p->ctx,argv[1]),p->last_error(p->ctx));
            check(p->commit(p->ctx,argv[4]),p->last_error(p->ctx));
            check(!nes_mod_hd_pack(),"imported pack enabled itself");
            check(p->feature_enable(p->ctx,argv[5],"hd-pack",1),p->last_error(p->ctx));
            check(p->commit(p->ctx,argv[4]),p->last_error(p->ctx));
            check(nes_mod_hd_pack(),"installed pack not committed");
            std::cout<<"PASS: actual archive installed, default-off, then selected and verified through Mods\n";return 0;
        } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
    }
    const fs::path root=fs::absolute("hd-package-fixtures");
    const fs::path dir=root/"packages/test.hd/1.0.0";
    try {
        fs::create_directories(dir/"pack/subdir");
        fs::remove(root/"state.toml");
        std::ofstream(dir/"pack/hires.txt")<<"<ver>106\n<scale>2\n";
        std::ofstream(dir/"pack/subdir/art.png")<<"fixture bytes";
        std::vector<uint8_t> image(16400,0);
        std::memcpy(image.data(),"NES\x1a",4);image[4]=1;
        write(root/"stock.nes",image);
        auto manifest=[&](const char* path="pack",bool legacy_patch=false) {
            std::ofstream f(dir/"manifest.toml");
            f<<"format_version=1\nid=\"test.hd\"\nversion=\"1.0.0\"\nname=\"HD\"\n"
               "[[target]]\ngame_id=\"test-game\"\nrom_crc32=\""<<crc(image)<<"\"\n"
               "[[feature]]\nid=\"art\"\nname=\"Art\"\ndefault_enabled=true\n"
               "exclusive_group=\"display-mode\"\n"
               "[[hd_pack]]\nfeature=\"art\"\ndirectory=\""<<path<<"\"\n";
            if(legacy_patch)f<<"patch=\"sound.ips\"\npatched_rom_crc32=\"01234567\"\n";
        };
        auto init=[&]() { std::string e;bool ok=NESRecomp::mod_runtime_initialize(root,"test-game",crc(image),&e);return ok; };
        std::string error;
        manifest();check(init(),"initial catalog");
        check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"unsupported executable accepted HD pack");
        check(nes_mod_hd_pack()==nullptr,"failed plan exposed HD assets");
        check(nes_mod_register_activation_plugin("nesrecomp.hd-pack",activate),"shared registration");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        const NESModHdPack *pack=nes_mod_hd_pack();
        check(pack && fs::path(pack->directory)==dir/"pack","asset-only package");
        uint8_t identity[20];std::memcpy(identity,pack->fingerprint,20);
        std::ofstream(dir/"pack/subdir/art.png",std::ios::app)<<" changed";
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        check(std::memcmp(identity,nes_mod_hd_pack()->fingerprint,20)!=0,"asset edits did not change identity");
        pack=nes_mod_hd_pack();
        nes_mod_lock_hd_pack();
        check(nes_mod_feature_requires_restart("test.hd","art"),"running pack was editable");
        const auto *provider=NESRecomp::mod_runtime_launcher_provider();
        check(!provider->feature_enable(provider->ctx,"test.hd","art",0),"live pack toggle succeeded");
        check(!provider->commit_netplay(provider->ctx,(root/"stock.nes").string().c_str()),"running HD pack accepted vanilla online plan");
        check(nes_mod_hd_pack()==pack,"rejected toggle changed committed assets");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        manifest("pack",true);check(init(),"legacy catalog");
        check(!provider->feature_enable(provider->ctx,"test.hd","art",1),"legacy pack was enabled");
        check(std::strstr(provider->last_error(provider->ctx),"native compatibility provider"),
              "missing compatibility status");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error) && !nes_mod_hd_pack(),
              "legacy pack blocked stock play or exposed HD assets");
        std::vector<uint8_t> reread(image.size());std::ifstream f(root/"stock.nes",std::ios::binary);f.read((char*)reread.data(),reread.size());f.close();
        check(reread==image,"original ROM changed");
        manifest("../outside");check(!init(),"parent path accepted");
        manifest("C:/outside");check(!init(),"drive path accepted");
        manifest();fs::remove(root/"state.toml");check(init(),"restored catalog");
        fs::remove(dir/"pack/hires.txt");
        check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"missing manifest accepted");
        fs::remove_all(root); // Exact fixture directory created by this test.
        std::cout<<"PASS: shared HD assets, legacy patch refusal, fingerprints and restart guard\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
}
