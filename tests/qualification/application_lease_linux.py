"""Run only inside the namespace wrapper, with the app-streams fixture running."""
import json
import socket
import time


def control(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        client.connect('/run/graphlab-node.sock')
        client.sendall((command + '\n').encode())
        data = b''
        while not data.endswith(b'\n'):
            chunk = client.recv(4096)
            assert chunk, 'gate closed'
            data += chunk
        result = json.loads(data)
        assert 'error' not in result, result
        return result


for partial in (False, True):
    control('release-lease')
    # Start after acknowledgement so sending at +10.15 is certainly after expiry.
    start = time.monotonic()
    time.sleep(max(0, 9.80 - (time.monotonic() - start)))
    with socket.create_connection(('10.233.99.1', 49001), timeout=2) as peer:
        if partial:
            peer.sendall(b'A' * 8)
        time.sleep(max(0, 10.15 - (time.monotonic() - start)))
        reply = b''
        try:
            peer.sendall(b'A' * (8 if partial else 16))
            reply = peer.recv(16)
        except (BrokenPipeError, ConnectionResetError):
            pass
        assert reply == b'', 'payload echoed after traffic lease expiry'
    status = control('status')
    assert status['state'] == 'held' and not status['leaseActive'], status
    alpha = status['applicationEdgeTelemetry'][0]
    assert alpha['counters']['receivedMessages'] == '0', alpha
    print(json.dumps({'partial': partial, 'elapsedSeconds': round(time.monotonic()-start, 3),
                      'echoBytes': len(reply), 'state': status['state']}), flush=True)

# Ordinary unleased release still permits both streams, then quiescence closes them.
control('release')
for tag in (b'A', b'B'):
    with socket.create_connection(('10.233.99.1', 49001), timeout=2) as peer:
        peer.sendall(tag * 16)
        reply = b''
        while len(reply) < 16:
            chunk = peer.recv(16-len(reply))
            assert chunk
            reply += chunk
        assert reply == tag * 16
assert control('quiesce')['state'] == 'held'
print('PASS delayed/partial expiry, unleased alpha/beta and quiescence')
