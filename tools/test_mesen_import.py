"""Exercise native Mods import of owner downloads through content-free recipes."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import stat
import subprocess
import time
import zipfile
import zlib


def zip_bytes(files):
    stream=io.BytesIO()
    with zipfile.ZipFile(stream,'w',zipfile.ZIP_DEFLATED) as archive:
        for name,data in files.items(): archive.writestr(name,data)
    return stream.getvalue()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    for name in ('zelda-pack','zelda-rom','smb-pack','smb-rom'): parser.add_argument('--'+name,type=Path)
    args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    image=b'NES\x1a'+bytes([1,0])+bytes(10)+bytes(16384)
    rom=out/'stock.nes';rom.write_bytes(image)
    original_hash=hashlib.sha1(image).hexdigest()
    patch=b'PATCH'+bytes([0,0,16,0,1,0xa9])+b'EOF'
    files={'hires.txt':f'<ver>106\n<scale>2\n<supportedRom>{original_hash}\n'.encode(),
           'art.png':b'synthetic pixels','LICENSE.txt':b'creator terms'}
    si=None
    if hasattr(subprocess,'STARTUPINFO'):
        si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0
    results=[]
    def recipe(name,game,image,patched='',require_patch=False):
        text=f'''format_version=1
id="test.mesen"
version="2.0.0"
name="User Mesen pack"
resolver="declarative"
[[target]]
game_id="{game}"
rom_crc32="{zlib.crc32(image[16:]):08x}"
[[feature]]
id="hd-pack"
name="Mesen HD"
default_enabled=false
exclusive_group="display-mode"
[[hd_pack]]
feature="hd-pack"
source="mesen"
resource_id="source-pack"
label="Original Mesen HD download"
require_patch={str(require_patch).lower()}
'''+(f'patched_rom_crc32="{patched}"\n' if patched else '')
        path=out/(name+'-support.nesmod');path.write_bytes(zip_bytes({'manifest.toml':text.encode()}))
        with zipfile.ZipFile(path) as archive: assert archive.namelist()==['manifest.toml']
        return path
    support=recipe('synthetic','arbitrary-game',image)
    def run(name,source,mode='import',support=support,rom=rom,game='arbitrary-game',catalog=None):
        catalog=catalog or out/(name+'-mods')
        start=time.monotonic()
        p=subprocess.run([str(args.driver.resolve()),mode,str(support),str(catalog),game,str(rom),str(source)],
            cwd=out,capture_output=True,text=True,startupinfo=si,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        elapsed=time.monotonic()-start
        (out/(name+'.log')).write_text(p.stdout+p.stderr)
        assert p.returncode==0,(name,p.stdout,p.stderr)
        results.append({'case':name,'seconds':round(elapsed,3)})
        print(name,'PASS',round(elapsed,2),'seconds',flush=True)
        return p.stdout,catalog
    direct=out/'UnrelatedName.zip';direct.write_bytes(zip_bytes(files))
    run('direct',direct)
    subdir=out/'Subfolder.zip';subdir.write_bytes(zip_bytes({'Release/Pack/'+n:d for n,d in files.items()}))
    run('subfolder',subdir)
    wrapper=out/'DownloadFirst.zip'
    wrapper.write_bytes(zip_bytes({'AnyInternalName.zip':direct.read_bytes(),'Creator.txt':b'outer creator notice'}))
    output,catalog=run('nested',wrapper)
    imported=next((catalog/'imports').glob('*/pack'))
    assert (imported/'art.png').read_bytes()==files['art.png']
    assert any(p.read_bytes()==b'outer creator notice' for p in imported.parent.joinpath('notices').iterdir())
    wrapper.unlink()
    run('resume-without-download',wrapper,'resume',catalog=catalog)
    patch_tag=f'<patch>fix.ips,{original_hash}\n'.encode()
    (imported/'hires.txt').write_bytes(files['hires.txt']+patch_tag)
    (imported/'fix.ips').write_bytes(patch)
    run('cached-memory-overlay',wrapper,'resume-patch',catalog=catalog)
    folder=out/'directory-pack';folder.mkdir()
    for name,data in files.items():(folder/name).write_bytes(data)
    run('selected-hires',folder/'hires.txt')
    legacy=out/'legacy.zip'
    legacy.write_bytes(zip_bytes({**files,'hires.txt':files['hires.txt']+patch_tag,'fix.ips':patch}))
    run('direct-memory-overlay',legacy,'patch')
    old_recipe=recipe('old','arbitrary-game',image,require_patch=True)
    run('required-patch-missing',direct,'missing-patch',support=old_recipe)
    run('required-patch-present',legacy,'patch',support=old_recipe)
    for index,name in enumerate(('../escaped','/absolute','C:/escape','dir/../escape','NUL.png','HIRES.TXT')):
        path=out/f'unsafe{index}.zip';path.write_bytes(zip_bytes({**files,name:b'bad'}))
        run(f'unsafe-{index}',path,'reject')
    path=out/'symlink.zip'
    with zipfile.ZipFile(path,'w') as archive:
        archive.writestr('hires.txt',files['hires.txt'])
        info=zipfile.ZipInfo('link');info.external_attr=(stat.S_IFLNK|0o777)<<16
        archive.writestr(info,'../target')
    run('symlink',path,'reject')
    path=out/'reparse.zip'
    with zipfile.ZipFile(path,'w') as archive:
        archive.writestr('hires.txt',files['hires.txt'])
        info=zipfile.ZipInfo('link');info.external_attr=0x400
        archive.writestr(info,'target')
    run('reparse',path,'reject')
    path=out/'ambiguous.zip';path.write_bytes(zip_bytes({'one.zip':direct.read_bytes(),'two.zip':direct.read_bytes()}))
    run('ambiguous',path,'reject')
    path=out/'corrupt.zip';path.write_bytes(direct.read_bytes()[:30]);run('truncated',path,'reject')
    path=out/'deep.zip';nested=direct.read_bytes()
    for _ in range(3):nested=zip_bytes({'next.zip':nested})
    path.write_bytes(nested);run('depth-limit',path,'reject')
    other=out/'other-original.nes'
    other.write_bytes(b'NES\x1a'+bytes([1,0,0,0,1])+bytes(7)+bytes(16384))
    run('base-sha-refusal',direct,'bad-base',rom=other)
    assert rom.read_bytes()==image
    if args.zelda_pack and args.zelda_rom:
        source_hash=hashlib.sha256(args.zelda_pack.read_bytes()).hexdigest()
        actual_rom=args.zelda_rom.read_bytes()
        zelda=recipe('zelda','legend-of-zelda-us',actual_rom)
        run('real-zelda-memory-overlay',args.zelda_pack,support=zelda,rom=args.zelda_rom,game='legend-of-zelda-us')
        assert hashlib.sha256(args.zelda_pack.read_bytes()).hexdigest()==source_hash
        assert args.zelda_rom.read_bytes()==actual_rom
    if args.smb_pack and args.smb_rom:
        smb=recipe('smb','super-mario-bros-us',args.smb_rom.read_bytes())
        run('real-smb',args.smb_pack,support=smb,rom=args.smb_rom,game='super-mario-bros-us')
    (out/'results.json').write_text(json.dumps(results,indent=2))


if __name__=='__main__':main()
