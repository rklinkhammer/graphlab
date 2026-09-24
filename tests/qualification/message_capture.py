"""Independent, read-only GLM1/v1 packet reference verification."""
import hashlib,json,struct,sys
from pathlib import Path
root,run,event,artifact,block,packet_index=sys.argv[1:]
expected=json.loads(event)['observation']
for path in Path(root).rglob('manifest.json'):
 m=json.loads(path.read_text())
 if m.get('runId')!=run:continue
 for s in m['segments']:
  if f"{m['id']}-{s['sequence']}"!=artifact:continue
  data=(path.parent/s['file']).read_bytes()
  assert 'sha256:'+hashlib.sha256(data).hexdigest()==s['sha256']
  offset=0;index=-1
  while offset<len(data):
   kind,size=struct.unpack_from('<II',data,offset)
   assert struct.unpack_from('<I',data,offset+size-4)[0]==size
   if kind==6:index+=1
   if offset==int(block):
    assert kind==6 and index==int(packet_index)
    captured,original=struct.unpack_from('<II',data,offset+20);assert captured==original
    raw=data[offset+28:offset+28+captured];pos=14;ether=struct.unpack_from('!H',raw,12)[0]
    for _ in range(2):
     if ether not in (0x8100,0x88a8):break
     ether=struct.unpack_from('!H',raw,pos+2)[0];pos+=4
    assert ether==0x800 and raw[pos+9]==17 and struct.unpack_from('!H',raw,pos+6)[0]&0x3fff==0
    udp=pos+(raw[pos]&15)*4;length=struct.unpack_from('!H',raw,udp+4)[0]
    wire=raw[udp+8:udp+length];assert length==61 and len(wire)==53 and wire[:4]==b'GLM1'
    assert wire[4:52].decode()==expected['messageId']
    assert ('alpha' if wire[-1:] in (b'A',b'a') else 'beta')==expected['stream']
    assert ('request' if wire[-1:] in (b'A',b'B') else 'response')==expected['phase']
    print(json.dumps(dict(artifactId=artifact,packetIndex=index,blockOffset=offset,wireIdentifierVerified=True,sha256=s['sha256'])));sys.exit(0)
   offset+=size
raise AssertionError('exact packet reference not found')
