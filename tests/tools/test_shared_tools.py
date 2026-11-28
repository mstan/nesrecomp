import contextlib, importlib.util, io, json, os, socket, subprocess, sys, tempfile, threading, unittest, zlib
from unittest import mock
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TEMP=Path(tempfile.gettempdir())
def module(name):
    spec=importlib.util.spec_from_file_location('nes_'+name,ROOT/'tools'/f'{name}.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

class SharedTools(unittest.TestCase):
    @unittest.skipUnless(os.name=='nt','Windows generator contract')
    def test_windows_cached_ninja_plan(self):
        builder=module('build_cycle');commands=[]
        class Process:
            stdout=[]
            def __init__(self,command,**kwargs):commands.append(command)
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def wait(self):return 0
        with tempfile.TemporaryDirectory(dir=TEMP) as tmp:
            root=Path(tmp);image=root/'image.nes';image.write_bytes(b'original fixture')
            build=root/'build';build.mkdir();(build/'CMakeCache.txt').write_text('CMAKE_GENERATOR:INTERNAL=Ninja\n')
            with mock.patch.object(builder,'windows_toolchain',return_value=('Visual Studio 17 2022',Path(sys.executable))),mock.patch.object(builder.subprocess,'Popen',Process),mock.patch.object(sys,'argv',['build_cycle.py','--project-root',str(root),'--rom',str(image),'--build-dir',str(build),'--jobs','2']):
                builder.main()
            self.assertEqual(commands[0][commands[0].index('-G')+1],'Ninja')
            self.assertNotIn('-A',commands[0]);self.assertIn('-DCMAKE_BUILD_TYPE=Release',commands[0])
            self.assertIn('-DNESRECOMP_PPU_IMPL:STRING=HLE',commands[0])
            self.assertEqual(commands[1][-2:],['--parallel','2'])
    def test_fragmented_transport_and_multiple_lines(self):
        dbg=module('dbg'); got=[]
        server=socket.socket();server.bind(('127.0.0.1',0));server.listen()
        def serve():
            with server:
                conn,_=server.accept()
                with conn:
                    got.append(json.loads(conn.makefile('rb').readline()))
                    conn.sendall(b'{"ok":');conn.sendall(b'true}\n{"frame":7}\n')
        thread=threading.Thread(target=serve);thread.start()
        out=io.StringIO()
        with contextlib.redirect_stdout(out):dbg.send_cmd('ping',_host='127.0.0.1',_port=server.getsockname()[1],host='payload-host')
        thread.join();self.assertEqual(got,[{'id':1,'cmd':'ping','host':'payload-host'}]);self.assertIn('true',out.getvalue())
    def test_transport_eof(self):
        dbg=module('dbg'); server=socket.socket();server.bind(('127.0.0.1',0));server.listen();port=server.getsockname()[1]
        def serve():
            with server:
                conn,_=server.accept()
                with conn:conn.recv(100);conn.sendall(b'{"error":"no command"}')
        thread=threading.Thread(target=serve);thread.start();out=io.StringIO()
        with contextlib.redirect_stdout(out):dbg.send_cmd('bad',_port=port)
        thread.join();self.assertIn('no command',out.getvalue())
    def test_profile_guards(self):
        with tempfile.TemporaryDirectory(dir=TEMP) as tmp:
            path=Path(tmp)/'synthetic.nes';payload=bytes(16384)
            def check(raw,mapper=0,good=True):
                path.write_bytes(raw)
                r=subprocess.run([sys.executable,str(ROOT/'tools/verify_usa_rom.py'),'--rom',str(path),'--crc',f'{zlib.crc32(payload):08x}','--mapper',str(mapper),'--title','Synthetic'],capture_output=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                self.assertEqual(r.returncode==0,good,r.stderr)
            header=bytearray(b'NES\x1a'+bytes(12));header[4]=1
            check(header+payload)
            header[6]=4;check(header+bytes(512)+payload)
            header[7]=8;header[8]=1;check(header+bytes(512)+payload,256)
            check(header+bytes(512)+payload[:-1],256,False)
            check(header+bytes(512)+bytes([1])+payload[1:],256,False)
            check(b'bad',good=False)
    def test_migration_bank_ambiguity_and_guards(self):
        m=module('migrate_hints')
        with tempfile.TemporaryDirectory(dir=TEMP) as tmp:
            old,new=Path(tmp)/'old.sym',Path(tmp)/'new.sym'
            old.write_text('[labels]\n00:8000 Begin\n00:8010 End\n00:8020 Gone\n01:8000 Begin\n')
            new.write_text('[labels]\n00:8100 Begin\n00:8110 End\n01:8200 Begin\n')
            mapper=m.Mapper(old,new,7,0xc000,8)
            self.assertEqual(mapper.map_addr(0,0x8000)[:2],(0x8100,'moved'))
            self.assertEqual(mapper.map_addr(0,0x8005)[:2],(0x8105,'interpolated'))
            self.assertEqual(mapper.map_addr(-1,0x8000)[1],'ambiguous')
            self.assertEqual(mapper.map_addr(0,0x8020)[1],'label-gone')
            rep=m.Report();text='[functions]\nbank0 = [0x8000, 0x8020]\n'
            migrated=m.migrate(text,mapper,rep)
            self.assertIn('0x8100',migrated);self.assertNotIn('0x8020',migrated)
    def test_migration_dropped_label_and_trainer_span(self):
        m=module('migrate_hints')
        with tempfile.TemporaryDirectory(dir=TEMP) as tmp:
            root=Path(tmp);old,new=root/'old.sym',root/'new.sym'
            old.write_text('[labels]\n00:8000 Begin\n00:8004 Gone\n00:8010 End\n')
            new.write_text('[labels]\n00:8000 Begin\n00:8010 End\n')
            mapper=m.Mapper(old,new,7,0xc000,8)
            self.assertEqual(mapper.map_addr(0,0x8004)[1],'label-gone')
            header=bytearray(b'NES\x1a'+bytes(12));header[4]=1;header[6]=4
            image=root/'trainer.nes';image.write_bytes(header+bytes([0xaa])*512+bytes([0xea])*16384)
            self.assertEqual(m.RomImage(image,7,0xc000).slice(0,0x8000,0x8010),bytes([0xea])*16)
    def test_release_file_guards(self):
        support=module('release_support')
        for path in ['foo.nes','mods/state.toml','a.cycstate','rom.cfg','nested/crash.dmp']:
            with self.assertRaises(RuntimeError):support.check_files([path])
        support.check_files(['assets/font.ttf','licenses/LICENSE.txt','mods/packages/a/1/manifest.toml'])

if __name__=='__main__':unittest.main(verbosity=2)
