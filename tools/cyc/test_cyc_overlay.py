"""Exercise actual game-owned ImGui overlay pause and device navigation."""
import argparse,json,subprocess,sys,time
from pathlib import Path
from test_cyc_window import Window,OPEN,NO_WINDOW
ap=argparse.ArgumentParser();ap.add_argument('--window',required=True,type=Path);ap.add_argument('--out',required=True,type=Path);a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(Path(__file__).with_name('fds_hle_fixtures.py')),'--out',str(out/'fx')],capture_output=True,creationflags=NO_WINDOW,check=True)
w=Window(a.window.resolve(),out/'fx/disk2_2.fds',out/'fx/bios/hle.rom',out,'game-overlay')
try:
 w.wait_frames(30);w.ok('overlay',open=True);time.sleep(.15);before=w.state()['frame'];time.sleep(.15);assert w.state()['frame']==before,'overlay did not pause the game'
 w.ok('key',name='Tab',frames=2);time.sleep(.15);w.ok('key',name='Return',frames=2);time.sleep(.15);first=w.ok('overlay');assert first['selected']==1,first
 w.ok('key',name='Tab',frames=2);time.sleep(.15);w.ok('key',name='Return',frames=2);time.sleep(.15);keyboard=w.ok('overlay');assert keyboard['selected']==2,keyboard
 w.ok('pad',buttons='dpup',frames=2);time.sleep(.15);w.ok('pad',buttons='a',frames=2);time.sleep(.15);controller=w.ok('overlay');assert controller['selected']==1,controller
 w.ok('screenshot',layer='ui',path=str(out/'overlay.png'));w.ok('overlay',open=False);w.wait_frames(10)
 (out/'result.json').write_text(json.dumps(dict(pause=True,keyboard=keyboard,controller=controller,resume=True),indent=2));assert w.quit()==0
finally:
 for p in OPEN:
  if p.poll() is None:p.terminate();p.wait(timeout=10)
