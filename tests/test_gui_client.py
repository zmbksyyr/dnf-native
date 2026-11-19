"""Exercise actual GM launch and optional client synchronization on file-only fixtures."""
import argparse,configparser,ctypes as c,datetime,hashlib,json,os,shutil,subprocess,sys,time
from pathlib import Path
from ctypes import wintypes as w
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from bootstrap import BUILD,sha256
from gui_support import user,kernel,windows,children,pid_of,title,click,select_folder,wait_for,screenshot,log_text
from test_login import external_test_key

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--gui',type=Path,required=True);p.add_argument('--source-instance',type=Path,required=True);p.add_argument('--client',type=Path,required=True);args=p.parse_args()
 repo=Path(__file__).resolve().parents[1];gui=args.gui.resolve(strict=True);source=args.source_instance.resolve(strict=True);original=args.client.resolve(strict=True)
 work=BUILD/'probes'/datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')/'gui-client';work.mkdir(parents=True)
 instance=work/'instance';game=instance/'home/neople/game';game.mkdir(parents=True)
 shutil.copy2(source/'services.ini',instance/'services.ini');shutil.copy2(source/'home/neople/game/Script.pvf',game/'Script.pvf')
 for name in('privatekey.pem','publickey.pem'):shutil.copy2(source/'home/neople/game'/name,game/name)
 client=work/'client';client.mkdir();shutil.copy2(original/'Script.pvf',client/'Script.pvf')
 for name in('private_key.pem','publickey.pem'):shutil.copy2(original/name,client/name)
 # A disposable nondefault client key exercises precedence and rollback even
 # when the selected source client happens to use our built-in pair.
 (client/'private_key.pem').write_bytes(external_test_key()[0]);(client/'publickey.pem').unlink()
 original_hashes={p.name:sha256(p)for p in(original/'Script.pvf',original/'private_key.pem',original/'publickey.pem')}
 initial_game=sha256(game/'Script.pvf');record={'checks':{},'scope':'Real Win32 GM button and client file import, comparison, failure rollback and reopening; no database or game process started','os':sys.getwindowsversion().build,'gui_sha256':sha256(gui)};checks=record['checks'];process=None;gm=None
 def save():(work/'receipt.json').write_text(json.dumps(record,indent=2))
 def cfg():
  ini=configparser.ConfigParser(interpolation=None);ini.read(instance/'services.ini',encoding='utf16');return ini
 def start():
  nonlocal process
  process=subprocess.Popen([str(gui),'--instance',str(instance)],env=dict(os.environ,LOCALAPPDATA=str(work/'profile')))
  return wait_for(lambda:next(iter(windows(process.pid,'DNFNativeController')),None),'controller')
 def open_features(h):
  click(h,300);m=wait_for(lambda:next(iter(windows(process.pid,'DnfNativeMarketMaintenance')),None),'features')
  wait_for(lambda:'检查中'not in title(user.GetDlgItem(m,295)),'client status');return m
 def select(m,path):
  owner=next(iter(windows(process.pid,'DNFNativeController')));user.SendMessageW(owner,0x111,284,0);before=title(user.GetDlgItem(owner,206))
  click(m,291);select_folder(process,path)
  wait_for(lambda: title(user.GetDlgItem(owner,206))!=before and any(text in title(user.GetDlgItem(owner,206))[len(before):]for text in('客户端同步完成','操作失败')),'client operation result',120)
  wait_for(lambda:user.IsWindowEnabled(user.GetDlgItem(m,291)),'client import finished',120)
 def close(h):
  user.PostMessageW(h,0x10,0,0);process.wait(timeout=15)
 try:
  h=start();m=open_features(h)
  assert '未关联'in title(user.GetDlgItem(m,295))and'未关联'in title(user.GetDlgItem(m,296));checks['optional_without_client']=True
  select(m,client)
  assert sha256(game/'Script.pvf')==sha256(client/'Script.pvf')and initial_game!=sha256(game/'Script.pvf')
  assert (game/'privatekey.pem').read_bytes()==(client/'private_key.pem').read_bytes()
  assert cfg().get('client','directory')==str(client)
  assert '一致'in title(user.GetDlgItem(m,295))and'一致'in title(user.GetDlgItem(m,296));checks['real_folder_selection_imports_pvf_and_client_keys']=True
  assert any(p.name.endswith('.before')and sha256(p)==initial_game for p in(instance/'.mysql').rglob('*')if p.is_file());checks['previous_pvf_backup_verified']=True
  screenshot(m,work/'features.png')
  # External edits must change the actual indicator, including missing keys.
  with(client/'Script.pvf').open('ab')as f:f.write(b'fixture change')
  wait_for(lambda:'不一致'in title(user.GetDlgItem(m,295)),'changed PVF indicator',20)
  click(m,292);wait_for(lambda:user.IsWindowEnabled(user.GetDlgItem(m,292))and title(user.GetDlgItem(m,295))=='● PVF 一致','repeat sync',120)
  (client/'private_key.pem').rename(client/'private-key.saved')
  wait_for(lambda:'无法核对'in title(user.GetDlgItem(m,296)),'missing key indicator',20)
  (client/'private-key.saved').rename(client/'private_key.pem');wait_for(lambda:title(user.GetDlgItem(m,296))=='● 密钥 一致','key restored',20);checks['indicators_follow_external_changes']=True
  # Invalid/mismatched client keys must fail before touching the instance.
  broken=work/'broken';broken.mkdir();shutil.copy2(client/'Script.pvf',broken/'Script.pvf');(broken/'private_key.pem').write_bytes(b'invalid')
  before={p:sha256(p)for p in(game/'Script.pvf',game/'privatekey.pem',game/'publickey.pem',instance/'services.ini')}
  select(m,broken);assert all(sha256(p)==v for p,v in before.items());checks['invalid_key_preserves_instance']=True
  # A locked key forces failure after the new PVF was committed: restore it.
  empty=work/'empty';empty.mkdir();shutil.copy2(original/'Script.pvf',empty/'Script.pvf')
  for name in('privatekey.pem','publickey.pem'):shutil.copy2(repo/'resources/login'/('private_key.pem'if name=='privatekey.pem'else name),game/name)
  assert sha256(game/'privatekey.pem')!=sha256(client/'private_key.pem'), 'Rollback fixture needs a different client key'
  wait_for(lambda:title(user.GetDlgItem(m,296))=='● 密钥 不一致','default server key observed',20)
  locked_before={p:sha256(p)for p in(game/'Script.pvf',game/'privatekey.pem',game/'publickey.pem',instance/'services.ini')}
  kernel.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE];kernel.CreateFileW.restype=w.HANDLE
  locked=kernel.CreateFileW(str(game/'privatekey.pem'),0x80000000,3,None,3,0,None)
  assert locked and locked!=w.HANDLE(-1).value
  try:select(m,client)
  finally:kernel.CloseHandle(locked)
  assert all(sha256(p)==v for p,v in locked_before.items());checks['mid_commit_failure_rolls_back_files_and_selection']=True
  select(m,empty);assert (empty/'private_key.pem').read_bytes()==(repo/'resources/login/private_key.pem').read_bytes()
  assert (game/'privatekey.pem').read_bytes()==(empty/'private_key.pem').read_bytes();assert title(user.GetDlgItem(m,296))=='● 密钥 一致';checks['missing_client_keys_use_default_pair_without_rename']=True
  user.PostMessageW(m,0x10,0,0);wait_for(lambda:not user.IsWindow(m),'close features');close(h)
  h=start();m=open_features(h);assert str(empty)==title(user.GetDlgItem(m,294));assert title(user.GetDlgItem(m,295))=='● PVF 一致'and title(user.GetDlgItem(m,296))=='● 密钥 一致';checks['selection_and_equality_survive_reopening']=True
  user.PostMessageW(m,0x10,0,0);wait_for(lambda:not user.IsWindow(m),'close features')
  # Click the independent binary and verify its process owns a real GM window.
  before_pids=set(subprocess.check_output(['powershell.exe','-NoProfile','-Command','(Get-Process DNF_GM -ErrorAction SilentlyContinue).Id'],text=True).split())
  click(h,290)
  def gm_window():
   found=[]
   @c.WINFUNCTYPE(w.BOOL,w.HWND,c.c_ssize_t)
   def visit(hwnd,unused):
    if title(hwnd).startswith('DNF')and pid_of(hwnd)!=process.pid and str(pid_of(hwnd))not in before_pids:
     handle=kernel.OpenProcess(0x1000,False,pid_of(hwnd))
     if handle:
      path=c.create_unicode_buffer(32768);size=w.DWORD(len(path))
      kernel.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)]
      if kernel.QueryFullProcessImageNameW(handle,0,path,c.byref(size))and Path(path.value).name.lower()=='dnf_gm.exe':found.append(hwnd)
      kernel.CloseHandle(handle)
    return True
   user.EnumWindows(visit,0);return next(iter(found),None)
  gm=wait_for(gm_window,'independent GM window',20);record['gm_pid']=pid_of(gm);checks['main_button_opens_real_independent_gm']=True
  user.PostMessageW(gm,0x10,0,0);gm=None;close(h)
  assert all(sha256(original/name)==v for name,v in original_hashes.items());checks['source_client_unchanged']=True
  record['passed']=True
 except Exception as error:
  record['error']=str(error)
  if process:
   for hwnd in windows(process.pid,'DNFNativeController'):record['failure_log']=title(user.GetDlgItem(hwnd,206))
  raise
 finally:
  save()
  if gm:user.PostMessageW(gm,0x10,0,0)
  if process and process.poll()is None:
   for hwnd in windows(process.pid,'DnfNativeMarketMaintenance'):user.SendMessageW(hwnd,0x10,0,0)
   for hwnd in windows(process.pid,'DNFNativeController'):user.PostMessageW(hwnd,0x10,0,0)
   process.wait(timeout=15)
  save()
 print('Client/GM GUI checks passed:',work,flush=True)
if __name__=='__main__':main()
