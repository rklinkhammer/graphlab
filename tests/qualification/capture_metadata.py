"""Independent read-only comparison of finalized Graphlab PCAPNG and manifests."""
import hashlib
import json
import struct
import sys
from pathlib import Path

result = []
for path in Path(sys.argv[1]).rglob('manifest.json'):
    manifest = json.loads(path.read_text())
    if manifest.get('runId') != sys.argv[2]:
        continue
    config = json.loads((path.parent / 'config.json').read_text())
    for segment in manifest['segments']:
        data = (path.parent / segment['file']).read_bytes()
        assert hashlib.sha256(data).hexdigest() == segment['sha256'].removeprefix('sha256:')
        offset = captured = original = truncated = packets = 0
        interface = None
        link_type = snaplen = None
        while offset < len(data):
            kind, size = struct.unpack_from('<II', data, offset)
            assert size >= 12 and offset + size <= len(data)
            assert struct.unpack_from('<I', data, offset + size - 4)[0] == size
            if kind == 1:
                link_type, _, snaplen = struct.unpack_from('<HHI', data, offset + 8)
                pos = offset + 16
                while pos < offset + size - 4:
                    code, length = struct.unpack_from('<HH', data, pos)
                    pos += 4
                    if not code:
                        break
                    if code == 2:
                        interface = data[pos:pos + length].decode()
                    pos += (length + 3) & ~3
            if kind == 6:
                cap, orig = struct.unpack_from('<II', data, offset + 20)
                captured += cap
                original += orig
                truncated += cap < orig
                packets += 1
            offset += size
        lengths = dict(apiVersion='graphlab.capture-lengths/v1', capturedBytes=str(captured), originalBytes=str(original), truncatedPackets=str(truncated))
        assert segment['packetLengths'] == lengths
        assert segment['packets'] == str(packets)
        assert segment['linkType'] == link_type == 1
        assert segment['snaplen'] == snaplen == config['snaplen']
        assert segment['interface'] == interface == config['interface']
        result.append(dict(id=f"{manifest['id']}-{segment['sequence']}", packetLengths=lengths, packets=str(packets), closedAt=segment['closedAt'], canonicalEndpoint=config['canonicalEndpoint'], captureInterface=interface, format=segment['format'], linkType=link_type, limits={k:config[k] for k in ('snaplen','byteBudget','rotateBytes','rotateSeconds')}))
assert result and sum(int(x['packets']) for x in result) > 0
print(json.dumps(result))
