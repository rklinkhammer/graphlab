"""Independent qualification decoder: Graphlab writer's finalized Ethernet PCAPNG only."""
import hashlib
import json
from pathlib import Path
import struct
import sys

root, run, begin, end = sys.argv[1:]
begin, end = float(begin), float(end)
counts = dict(alpha=0, beta=0)
references = []
for path in Path(root).glob('*/manifest.json'):
    manifest = json.loads(path.read_text())
    if manifest['runId'] != run or manifest['edge'] != 'b-s':
        continue
    assert manifest['state'] == 'closed'
    for segment in manifest['segments']:
        data = (path.parent / segment['file']).read_bytes()
        assert 'sha256:' + hashlib.sha256(data).hexdigest() == segment['sha256']
        offset, resolution = 0, 1e-6
        while offset < len(data):
            kind, size = struct.unpack_from('<II', data, offset)
            assert size >= 12 and offset + size <= len(data)
            assert struct.unpack_from('<I', data, offset + size - 4)[0] == size
            body = data[offset + 8:offset + size - 4]
            if kind == 1:
                assert struct.unpack_from('<H', body)[0] == 1
                pos = 8
                while pos + 4 <= len(body):
                    code, length = struct.unpack_from('<HH', body, pos)
                    if code == 0:
                        break
                    if code == 9:
                        value = body[pos + 4]
                        resolution = 2 ** -(value & 127) if value & 128 else 10 ** -value
                    pos += 4 + (length + 3) // 4 * 4
            if kind == 6:
                interface, high, low, captured, original = struct.unpack_from('<IIIII', body)
                timestamp = ((high << 32) + low) * resolution
                packet = body[20:20 + captured]
                if begin < timestamp < end and len(packet) >= 42:
                    ethernet = 14
                    ethertype = struct.unpack_from('!H', packet, 12)[0]
                    if ethertype == 0x8100:
                        ethernet = 18
                        ethertype = struct.unpack_from('!H', packet, 16)[0]
                    if ethertype == 0x800 and packet[ethernet + 9] == 17:
                        udp = ethernet + (packet[ethernet] & 15) * 4
                        destination = struct.unpack_from('!H', packet, udp + 2)[0]
                        payload = packet[udp + 8:]
                        if destination == 49000:
                            for name, byte in [('alpha', b'A'), ('beta', b'B')]:
                                if payload[:16] == byte * 16:
                                    counts[name] += 1
                                    references.append(dict(artifact=manifest['id'], file=segment['file'], blockOffset=offset))
            offset += size
assert references, 'No matching finalized packets in pause interval'
print(json.dumps(dict(**counts, references=references, interval=[begin, end])))
