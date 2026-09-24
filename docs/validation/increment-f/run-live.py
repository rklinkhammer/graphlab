import subprocess,os
from pathlib import Path
base=Path(__file__).resolve().parents[3];out=base/'docs/validation/increment-f';config=str(Path.home()/'.lima/graphlab/ssh.config');source='/tmp/graphlab-f-20260924'
def ssh(s):return subprocess.run(['ssh','-F',config,'lima-graphlab',s],check=True,capture_output=True,text=True)
for kind,file,flag,port in [('edge','application-edge','GRAPHLAB_APPLICATION_EDGE_LIVE',18095),('source','source-control','GRAPHLAB_SOURCE_CONTROL_LIVE',18096),('inspectors','inspectors','GRAPHLAB_INSPECTORS_LIVE',18098),('messages','messages','GRAPHLAB_MESSAGES_LIVE',18099)]:
 root='/var/tmp/graphlab-f-'+kind+'-20260924';agent='graphlab-f-'+kind+'-agent';api='graphlab-f-'+kind+'-api'
 print('START '+kind,flush=True)
 try:
  ssh(f'sudo systemd-run --unit={agent} --property=KillMode=process {source}/build/dev/lab-agent --socket {root}/rpc/agent.sock --topologies {root} --lock {root}/artifacts.lock.json --state {root}/state --allow-uid 501')
  ssh(f'sudo systemd-run --unit={api} --uid=501 {source}/build/dev/lab-api --socket {root}/rpc/agent.sock --auth {root}/auth.json --assets {source}/console/web/dist --port {port} --agent-uid 0')
  env={**os.environ,flag:'1','GRAPHLAB_LIVE_ROOT':root,'GRAPHLAB_LIVE_SOURCE':source,'GRAPHLAB_LIVE_AGENT':agent}
  with (out/(kind+'-live.txt')).open('w') as f:
   r=subprocess.run(['npm','test','--prefix','console/web','--','tests/'+file+'-live.spec.ts'],cwd=base,env=env,stdout=f,stderr=subprocess.STDOUT)
  print(kind+' exit='+str(r.returncode),flush=True)
  if r.returncode:raise SystemExit(r.returncode)
 finally: ssh(f'sudo systemctl stop {api} {agent}')
