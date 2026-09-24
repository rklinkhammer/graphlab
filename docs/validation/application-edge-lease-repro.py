import socket,time,json

def control(command):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.connect('/run/graphlab-node.sock');s.sendall((command+'\n').encode());data=b''
    while not data.endswith(b'\n'): data+=s.recv(4096)
    s.close();return json.loads(data)
start=time.monotonic(); control('release-lease')
time.sleep(max(0,9.80-(time.monotonic()-start)))
s=socket.create_connection(('10.233.99.1',49001),timeout=2)
time.sleep(max(0,10.15-(time.monotonic()-start)))
s.sendall(b'A'*16)
reply=s.recv(16)
print(json.dumps({'secondsAfterRelease':round(time.monotonic()-start,3),'echoAfterLeaseDeadline':reply==b'A'*16,'echoBytes':len(reply),'statusAfterEcho':control('status')['state']}))
s.close()
