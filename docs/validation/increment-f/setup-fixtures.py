from pathlib import Path
import json,shutil,subprocess
for kind,date in [('edge','20260923'),('source','20260923'),('inspectors','20260923'),('messages','20260924')]:
 old=Path('/var/tmp/gl6-'+kind+'-'+date);root=Path('/var/tmp/graphlab-f-'+kind+'-20260924');root.mkdir(exist_ok=False)
 for f in ['auth.json','password','artifacts.lock.json','topology.yaml']:
  shutil.copyfile(old/f,root/f)
  if f in ['auth.json','password']:(root/f).chmod(0o600)
 subprocess.check_call(['sudo','install','-d','-m','700',str(root/'state')]);subprocess.check_call(['sudo','install','-d','-m','750','-o','root','-g','1000',str(root/'rpc')])
 lock=json.loads((root/'artifacts.lock.json').read_text())
 for e in lock['workloads'].values():
  if 'image' in e:subprocess.check_call(['sudo','docker','image','inspect',e['image'].split('@')[1],'--format','{{.Id}}'])
 print(root)
