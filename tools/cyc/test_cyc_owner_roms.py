#!/usr/bin/env python3
"""Compile and compare a locally supplied ROM inventory without copying ROMs.

Manifest: JSON array of {"paths": ["/path/to/game.nes"], "sha256": "...",
"mapper": 1}. Paths must exist; the optional digest pins the input. No ROM or
commercial-game expectation is downloaded. This checks execution agreement on
the chosen input route, not whether a game can be completed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace
from cyc_verify import first_difference, run_error

def command(argv,log,cwd=None,timeout=300):
    p=subprocess.run([str(a) for a in argv],cwd=cwd,capture_output=True,text=True,
        timeout=timeout,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    output=p.stdout+p.stderr;log.write_text(output,encoding='utf-8',newline='\n')
    if p.returncode:raise RuntimeError(f'exit {p.returncode}: {log}')
    return output

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for flag in ('manifest','recompiler','interp','oracle','out'):ap.add_argument('--'+flag,type=Path,required=True)
    ap.add_argument('--frames',type=int,default=600)
    ap.add_argument('--input',type=Path)
    ap.add_argument('--mapper',type=int,action='append')
    ap.add_argument('--cmake',default='cmake')
    ap.add_argument('--generator')
    ap.add_argument('--jobs',type=int,default=4)
    ap.add_argument('--build-timeout',type=int,default=1800)
    ap.add_argument('--resume',action='store_true',help='Reuse completed runs only when their inputs, executable and outputs still match')
    args=ap.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if args.frames<=0 or args.jobs<=0:ap.error('frames and jobs must be positive')
    # Never leave a previous successful summary presented as the current run.
    (out/'results.json').write_text('[]\n',encoding='utf-8',newline='\n')
    source=Path(__file__).resolve().parents[2]/'runner/cyc'
    args.recompiler=args.recompiler.resolve();args.interp=args.interp.resolve();args.oracle=args.oracle.resolve()
    if not args.input:
        args.input=out/'input.txt'
        args.input.write_text('120 START\n121 -\n180 RIGHT\n200 RIGHT A\n210 RIGHT\n260 -\n',newline='\n')
    args.input=args.input.resolve()
    cases=[]
    for entry in json.loads(args.manifest.read_text(encoding='utf-8-sig')):
        if args.mapper and entry['mapper'] not in args.mapper:continue
        rom=next((Path(p).resolve() for p in entry['paths'] if Path(p).is_file()),None)
        if rom is None:raise FileNotFoundError(entry['paths'])
        digest=hashlib.sha256(rom.read_bytes()).hexdigest()
        if entry.get('sha256') and digest!=entry['sha256']:raise ValueError(f'changed input: {rom}')
        name=f'owner_{len(cases):02d}';folder=out/name;folder.mkdir(exist_ok=True)
        cases.append(dict(name=name,rom=str(rom),sha256=digest,mapper=entry['mapper'],folder=folder))
    if not cases:ap.error('no matching ROMs')
    def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    input_digest=digest(args.input)
    exe_digests={str(p):digest(p) for p in (args.interp,args.oracle)}
    def execute(case,align,mode,exe,extra=()):
        folder=case['folder'];trace=folder/f'a{align}_{mode}.txt'
        argv=[exe,case['rom'],'--frames',args.frames,'--align',align,'--input',args.input,'--hash-out',trace]
        if mode=='standalone':argv+=['--miss-log',folder/f'a{align}.seeds']
        log=trace.with_suffix('.log');stamp=trace.with_suffix('.complete.json')
        identity=dict(argv=[str(v) for v in argv+list(extra)],rom=case['sha256'],
            input=input_digest,executable=exe_digests[str(exe)])
        outputs=[trace,log]+([folder/f'a{align}.seeds'] if mode=='standalone' else [])
        if args.resume and stamp.exists() and all(p.is_file() for p in outputs):
            cached=json.loads(stamp.read_text())
            if cached==dict(identity=identity,outputs={p.name:digest(p) for p in outputs}):
                return log.read_text(encoding='utf-8')
        stamp.unlink(missing_ok=True);trace.unlink(missing_ok=True)
        output=command(argv+list(extra),trace.with_suffix('.log'))
        error=run_error(0,output,str(trace),SimpleNamespace(acccoin=False,frames=args.frames))
        if error:raise AssertionError(f'{case["name"]} {mode} alignment {align}: {error}')
        if mode=='native' and not re.search(r'native_cycles=[1-9][0-9]*\b',output):
            raise AssertionError(f'{case["name"]} alignment {align}: no native execution')
        stamp.write_text(json.dumps(dict(identity=identity,outputs={p.name:digest(p) for p in outputs})),encoding='utf-8',newline='\n')
        return output
    tasks=[(c,a,m,e) for c in cases for a in range(4) for m,e in [('standalone',args.interp),('oracle',args.oracle)]]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for _ in pool.map(lambda task:execute(*task),tasks):pass
    for c in cases:
        for a in range(4):
            diff=first_difference(c['folder']/f'a{a}_standalone.txt',c['folder']/f'a{a}_oracle.txt',False)
            if diff:raise AssertionError((c['rom'],a,diff))
        print(f'{c["name"]} mapper {c["mapper"]}: interpreter/oracle parity',flush=True)
    cmake=['cmake_minimum_required(VERSION 3.20)','project(owner_rom_matrix C)','set(CMAKE_C_STANDARD 11)',
        f'include("{source.as_posix()}/cyc.cmake")','add_library(owner_runtime OBJECT ${NESRECOMP_CYC_SOURCES})',
        'target_include_directories(owner_runtime PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
        'target_compile_definitions(owner_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)']
    for c in cases:
        folder=c['folder'];name=c['name'];seeds=set()
        for a in range(4):
            for line in (folder/f'a{a}.seeds').read_text().splitlines():
                if line and not line.startswith('#'):seeds.add(line.split()[0])
        (folder/'seeds.txt').write_text('\n'.join(sorted(seeds))+'\n',newline='\n')
        (folder/'game.toml').write_text(f'[game]\noutput_prefix="{name}"\ncycle_accurate=true\ncycle_seed_file="seeds.txt"\n',newline='\n')
        command([args.recompiler,c['rom'],'--game','game.toml'],folder/'codegen.log',cwd=folder)
        cmake += [f'file(GLOB {name}_GEN CONFIGURE_DEPENDS "{name}/generated/*_cyc*.c")',
            f'add_executable({name} $<TARGET_OBJECTS:owner_runtime> ${{{name}_GEN}})',
            f'target_include_directories({name} PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})',
            f'target_compile_definitions({name} PRIVATE _CRT_SECURE_NO_WARNINGS)',
            f'target_link_libraries({name} PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})']
    (out/'CMakeLists.txt').write_text('\n'.join(cmake)+'\n',newline='\n')
    configure=[args.cmake,'-S',out,'-B',out/'build','-DCMAKE_BUILD_TYPE=Release']
    if args.generator:configure+=['-G',args.generator]
    command(configure,out/'configure.log',timeout=args.build_timeout)
    command([args.cmake,'--build',out/'build','--config','Release','--parallel',args.jobs],out/'build.log',timeout=args.build_timeout)
    summary=[]
    for c in cases:
        suffix='.exe' if hasattr(subprocess,'CREATE_NO_WINDOW') else ''
        exe=out/'build'/'Release'/(c['name']+suffix)
        if not exe.exists():exe=out/'build'/(c['name']+suffix)
        exe_digests[str(exe)]=digest(exe)
        def compiled(task):
            a,mode=task;output=execute(c,a,mode,exe,['--interp-only'] if mode=='embedded' else [])
            diff=first_difference(c['folder']/f'a{a}_{mode}.txt',c['folder']/f'a{a}_standalone.txt',True)
            if diff:raise AssertionError((c['rom'],a,mode,diff))
            return dict(align=a,mode=mode,summary=re.search(r'^mode=.*$',output,re.M).group(0))
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results=list(pool.map(compiled,[(a,m) for a in range(4) for m in ('native','embedded')]))
        summary.append({k:v for k,v in c.items() if k!='folder'}|dict(frames=args.frames,runs=results))
        (out/'results.json').write_text(json.dumps(summary,indent=2),encoding='utf-8',newline='\n')
        print(f'{c["name"]} mapper {c["mapper"]}: all four execution modes agree at every alignment',flush=True)
    print(f'{len(cases)} ROMs / {len(cases)*16} completed executions / {args.frames} frames each; all traces match')

if __name__=='__main__':main()
