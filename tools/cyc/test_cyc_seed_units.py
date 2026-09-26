#!/usr/bin/env python3
"""Round-trip fine-bank miss logs, preserving legacy 8 KiB seed identities."""
import argparse
from pathlib import Path
import subprocess
from cyc_verify import first_difference
from fineprg_fixtures import fineprg_fixtures


def run(command, cwd, log):
    p = subprocess.run([str(x) for x in command], cwd=cwd, capture_output=True,
                       text=True, timeout=180,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    log.write_text(p.stdout + p.stderr)
    if p.returncode:
        raise AssertionError(f'{command[0]} failed: {log}')
    return p.stdout


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--recompiler',required=True,type=Path)
    ap.add_argument('--interp',required=True,type=Path)
    ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--cmake',default='cmake')
    ap.add_argument('--generator')
    args=ap.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    compiler,interp=args.recompiler.resolve(),args.interp.resolve()
    case=next(c for c in fineprg_fixtures() if c[0]=='fine31_slot1')
    rom=out/'test.nes';rom.write_bytes(case[1])
    seeds=out/'seeds.txt';seeds.write_text('00:9080 7\n')
    run([interp,rom,'--frames',3,'--miss-log',seeds,'--hash-out',out/'interp.txt'],
        out,out/'interp.log')
    lines=seeds.read_text().splitlines()
    assert '4k:01:9080 7' in lines, 'legacy 8 KiB bank 0 upper half lost its identity'
    assert any(line.startswith('4k:00:9080 ') for line in lines)
    assert any(line.startswith('4k:05:9087 ') for line in lines)
    # A second run must consume its own explicit-unit output and merge it.
    run([interp,rom,'--frames',3,'--miss-log',seeds],out,out/'merge.log')
    assert '4k:01:9080 7' in seeds.read_text().splitlines()
    (out/'game.toml').write_text('[game]\noutput_prefix="seed31"\ncycle_accurate=true\ncycle_seed_file="seeds.txt"\n')
    run([compiler,rom,'--game','game.toml'],out,out/'compiler.log')
    source=Path(__file__).resolve().parents[2]/'runner/cyc'
    (out/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(seed31 C)
include("{source.as_posix()}/cyc.cmake")
file(GLOB GEN CONFIGURE_DEPENDS "generated/*_cyc*.c")
add_executable(seed31 ${{NESRECOMP_CYC_SOURCES}} ${{GEN}})
target_include_directories(seed31 PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})
target_link_libraries(seed31 PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})
''')
    command=[args.cmake,'-S',out,'-B',out/'build','-DCMAKE_BUILD_TYPE=Release']
    if args.generator:command+=['-G',args.generator]
    run(command,out,out/'configure.log')
    run([args.cmake,'--build',out/'build','--config','Release','--parallel','4'],out,out/'build.log')
    native=out/'build/Release/seed31.exe' if hasattr(subprocess,'CREATE_NO_WINDOW') else out/'build/seed31'
    stdout=run([native,rom,'--frames',3,'--hash-out',out/'native.txt'],out,out/'native.log')
    assert '(100.0%)' in stdout
    assert first_difference(out/'interp.txt',out/'native.txt',True) is None
    print('legacy and 4 KiB seed identities, merge, recompilation, and native execution passed')


if __name__=='__main__':main()
