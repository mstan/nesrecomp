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
        std::ofstream(dir/"pack/hires.txt")<<"<ver>106\n<scale>2\n";
        std::ofstream(dir/"pack/subdir/art.png")<<"fixture bytes";
        std::vector<uint8_t> image(16400,0);
        std::memcpy(image.data(),"NES\x1a",4);image[4]=1;
        write(root/"stock.nes",image);
        std::vector<uint8_t> changed=image;changed[16]=0xa9;changed[17]=1;
        std::vector<uint8_t> ips={'P','A','T','C','H',0,0,16,0,2,0xa9,1,'E','O','F'};
        write(dir/"sound.ips",ips);
        auto manifest=[&](const char* path="pack",const std::string& patched="") {
            std::ofstream f(dir/"manifest.toml");
            f<<"format_version=1\nid=\"test.hd\"\nversion=\"1.0.0\"\nname=\"HD\"\n"
               "[[target]]\ngame_id=\"test-game\"\nrom_crc32=\""<<crc(image)<<"\"\n"
               "[[feature]]\nid=\"art\"\nname=\"Art\"\ndefault_enabled=true\n"
               "exclusive_group=\"display-mode\"\n"
               "[[hd_pack]]\nfeature=\"art\"\ndirectory=\""<<path<<"\"\n";
            if(!patched.empty())f<<"patch=\"sound.ips\"\npatched_rom_crc32=\""<<patched<<"\"\n";
        };
        auto init=[&]() { std::string e;bool ok=NESRecomp::mod_runtime_initialize(root,"test-game",crc(image),&e);return ok; };
        std::string error;
        manifest();check(init(),"initial catalog");
        check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"unsupported executable accepted HD pack");
        check(nes_mod_hd_pack()==nullptr,"failed plan exposed HD assets");
        check(nes_mod_register_activation_plugin("nesrecomp.hd-pack",activate),"shared registration");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        const NESModHdPack *pack=nes_mod_hd_pack();
        check(pack && !pack->patched_payload && fs::path(pack->directory)==dir/"pack","asset-only package");
        uint8_t identity[20];std::memcpy(identity,pack->fingerprint,20);
        std::ofstream(dir/"pack/subdir/art.png",std::ios::app)<<" changed";
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        check(std::memcmp(identity,nes_mod_hd_pack()->fingerprint,20)!=0,"asset edits did not change identity");
        manifest("pack",crc(changed));check(init(),"patched catalog");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        pack=nes_mod_hd_pack();check(pack && pack->payload_size==16384 && pack->patched_payload[0]==0xa9 && pack->patched_payload[1]==1,"IPS was not applied");
        std::vector<uint8_t> reread(image.size());std::ifstream f(root/"stock.nes",std::ios::binary);f.read((char*)reread.data(),reread.size());f.close();
        check(reread==image,"owner ROM changed");
        nes_mod_lock_hd_pack();
        check(nes_mod_feature_requires_restart("test.hd","art"),"running patch was editable");
        const auto *provider=NESRecomp::mod_runtime_launcher_provider();
        check(!provider->feature_enable(provider->ctx,"test.hd","art",0),"live patch toggle succeeded");
        check(!provider->commit_netplay(provider->ctx,(root/"stock.nes").string().c_str()),"running patched game accepted vanilla online plan");
        check(nes_mod_hd_pack()==pack,"rejected toggle changed committed assets");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        // Each new launch rechecks the actual installed bytes, not a UI cache.
        manifest("pack","00000000");check(init(),"bad CRC manifest");
        check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error) && !nes_mod_hd_pack(),"wrong patched CRC accepted");
        manifest("pack",crc(changed));
        for(const auto& bad : std::vector<std::vector<uint8_t>>{
            {'P','A','T','C','H',0,0,0,0,1,1,'E','O','F'}, // header edit
            {'P','A','T','C','H',0,0,16,0,2,0xa9}, // truncated record
            {'P','A','T','C','H','E','O','F',0,1,0}, // resized image
            {'P','A','T','C','H',255,255,255,0,1,1,'E','O','F'} // outside cartridge
        }) {
            write(dir/"sound.ips",bad);check(init(),"bad patch catalog");
            check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error) && !nes_mod_hd_pack(),"unsafe IPS accepted");
        }
        // RLE is a real IPS record type used by packs.
        ips={'P','A','T','C','H',0,0,16,0,0,0,2,0xa9,'E','O','F'};
        changed[17]=0xa9;write(dir/"sound.ips",ips);manifest("pack",crc(changed));check(init(),"RLE catalog");
        check(NESRecomp::mod_runtime_commit(root/"stock.nes",&error),error.c_str());
        check(nes_mod_hd_pack()->patched_payload[1]==0xa9,"RLE result");
        manifest("../outside");check(!init(),"parent path accepted");
        manifest("C:/outside");check(!init(),"drive path accepted");
        manifest();check(init(),"restored catalog");
        fs::remove(dir/"pack/hires.txt");
        check(!NESRecomp::mod_runtime_commit(root/"stock.nes",&error),"missing manifest accepted");
        fs::remove_all(root); // Exact fixture directory created by this test.
        std::cout<<"PASS: shared HD assets, patch verification, stock ROM preservation, fingerprints and live-toggle guard\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
}
