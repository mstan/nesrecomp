#!/usr/bin/env python3
"""Link independent generated programs and execute each on the real machine."""
import argparse
import json
from pathlib import Path
import subprocess
import nested_build


def run(command, cwd, log):
    result = subprocess.run([str(v) for v in command], cwd=cwd, capture_output=True,
                            text=True, timeout=180,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    log.write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise AssertionError(f'Command failed; see {log}')
    return result


def image(value):
    prg = bytearray([0xea] * 16384)
    prg[:6] = bytes([0xa9, value, 0x85, 0x00, 0x02, 0xea])
    prg[-6:] = bytes([0, 0x80] * 3)
    return b'NES\x1a' + bytes([1, 0]) + bytes(10) + prg


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--recompiler', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    nested_build.add_arguments(ap)
    args = ap.parse_args()
    tc = nested_build.Toolchain.from_args(args)
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    compiler = args.recompiler.resolve()
    for name, value in [('alpha', 0x11), ('beta', 0x22)]:
        folder = out / name; folder.mkdir(exist_ok=True)
        (folder / 'test.nes').write_bytes(image(value))
        (folder / 'game.toml').write_text('[game]\ncycle_namespace=' + json.dumps(name) + '\n')
        run([compiler, 'test.nes', '--game', 'game.toml', '--output-prefix', name], folder, folder / 'generate.log')
    source = Path(__file__).resolve().parents[2] / 'runner/cyc'
    (out / 'probe.c').write_text(r'''
#include "cyc_program.h"
#include "cyc_core.h"
#include "hw_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
CycProgram alpha_program(void);
CycProgram beta_program(void);
static int bad_catalog;
const CycProgram *cyc_program_catalog(size_t *count) {
    static CycProgram programs[2];
    static int initialized;
    if(!initialized) {
        programs[0]=alpha_program();programs[1]=bad_catalog?programs[0]:beta_program();
        initialized=1;
    }
    *count=2;return programs;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x);return 1; } } while(0)
static int check(CycProgram p,const char *file,unsigned value) {
    FILE *f=fopen(file,"rb");CHECK(f);
    unsigned char image[16400];CHECK(fread(image,1,sizeof image,f)==sizeof image);fclose(f);
    CHECK(cyc_load_ines(image,sizeof image));
    CHECK(cyc_native_prg_hash==p.prg_hash);cyc_ram_init=CYC_RAM_ZEROS;
    cyc_power_on(0);cpu_power_on();cpu_power_on_sequence();
    CHECK(cyc_native_has(0x8000));CHECK(!cyc_native_has(0x4000));
    cyc_native_run();CHECK(cpu.jammed);CHECK(cpu.a==value);CHECK(cyc_cpu_ram()[0]==value);
    unsigned char ram[2048];memcpy(ram,cyc_cpu_ram(),sizeof ram);
    unsigned pc=cpu.pc;
    cyc_power_on(0);cpu_power_on();cpu_power_on_sequence();
    for(unsigned i=0;i<8 && !cpu.jammed;i++)cpu_interp_step();
    CHECK(cpu.jammed && cpu.pc==pc && cpu.a==value);
    CHECK(!memcmp(ram,cyc_cpu_ram(),sizeof ram));
    image[17]^=0x80;
    CHECK(!cyc_load_ines(image,sizeof image));CHECK(cyc_native_prg_hash==p.prg_hash);
    CHECK(cyc_prg_hash()==p.prg_hash);CHECK(!memcmp(ram,cyc_cpu_ram(),sizeof ram));
    return 0;
}
int main(int argc,char **argv) {
    CycProgram a=alpha_program(),b=beta_program();
    if(argc==2 && !strcmp(argv[1],"--bad-catalog")) {
        bad_catalog=1;CHECK(!cyc_program_select(a.prg_hash,a.cart_hash));
        CHECK(!cyc_native_has(0x8000));return 0;
    }
    CHECK(argc==3);
    CHECK(a.prg_hash!=b.prg_hash);CHECK(a.cart_hash==b.cart_hash);
    CHECK(!strcmp(a.name,"alpha") && !strcmp(b.name,"beta"));
    CHECK(!strcmp(a.id,"alpha") && !strcmp(b.id,"beta"));
    CHECK(!a.ram_view_count && !b.ram_view_count);
    CHECK(cyc_program_select(a.prg_hash,a.cart_hash));
    uint32_t unknown=a.prg_hash+1;while(unknown==b.prg_hash)++unknown;
    CHECK(!cyc_program_select(unknown,a.cart_hash));CHECK(cyc_native_prg_hash==a.prg_hash);
    CHECK(!check(a,argv[1],0x11));CHECK(!check(b,argv[2],0x22));CHECK(!check(a,argv[1],0x11));
    return 0;
}
''', encoding='utf-8')
    (out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(programs C)
set(CMAKE_C_STANDARD 11)
include("{source.as_posix()}/cyc.cmake")
file(GLOB GEN CONFIGURE_DEPENDS "alpha/generated/*_cyc*.c" "beta/generated/*_cyc*.c")
set(BOARD "${{NESRECOMP_CYC_DIR}}/hw_mapper.c" "${{NESRECOMP_CYC_DIR}}/hw_fds.c"
    "${{NESRECOMP_CYC_DIR}}/hw_fds_audio.c" "${{NESRECOMP_CYC_DIR}}/hw_fds_hle.c"
    "${{NESRECOMP_CYC_DIR}}/vendor/emu2413/emu2413.c")
add_executable(program_probe probe.c ${{GEN}} ${{BOARD}}
    "${{NESRECOMP_CYC_DIR}}/cpu6502.c" "${{NESRECOMP_CYC_DIR}}/cpu6502_interp.c"
    "${{NESRECOMP_CYC_DIR}}/hw_machine.c" "${{NESRECOMP_CYC_DIR}}/hw_ppu.c"
    "${{NESRECOMP_CYC_DIR}}/hw_apu.c" "${{NESRECOMP_CYC_DIR}}/hw_palette.c"
    "${{NESRECOMP_CYC_DIR}}/cyc_trace.c" "${{NESRECOMP_CYC_DIR}}/cyc_ring.c"
    ${{NESRECOMP_CYC_SCHEDULER_SOURCES}} "${{NESRECOMP_CYC_DIR}}/cyc_program.c")
target_compile_definitions(program_probe PRIVATE CYC_NATIVE_PROGRAMS=1)
target_include_directories(program_probe PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})
target_link_libraries(program_probe PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})
''', encoding='utf-8')
    tc.build(out, out / 'build', out, timeout=900, parallel=1)
    run([tc.executable(out / 'build', 'program_probe'), out / 'alpha/test.nes', out / 'beta/test.nes'], out, out / 'probe.log')
    run([tc.executable(out / 'build', 'program_probe'), '--bad-catalog'], out, out / 'bad-catalog.log')
    for name in ['9bad', 'has space', 'bad-name', 'x' * 64]:
        folder = out / 'invalid'; folder.mkdir(exist_ok=True)
        config = folder / 'game.toml'; config.write_text('[game]\ncycle_namespace=' + json.dumps(name) + '\n')
        result = subprocess.run([str(compiler), str(out / 'alpha/test.nes'), '--game', str(config)],
                                cwd=folder, capture_output=True, text=True,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if result.returncode == 0:
            raise AssertionError(f'Invalid namespace accepted: {name}')
    print('Two generated programs linked; native/interpreter state matches for alpha/beta/alpha; invalid namespaces rejected.')


if __name__ == '__main__':
    main()
