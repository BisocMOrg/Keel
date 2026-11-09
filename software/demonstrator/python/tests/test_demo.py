# SPDX-License-Identifier: MIT
"""Portable safety, ordering and HTTP tests; no real board is opened."""
import http.client
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import Future

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from keel_demo import ApiError, Controller, DemoServer, Handler, clock_ms


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.app = Controller()
        self.seq = 0
        self.send('claim')

    def tearDown(self):
        self.app.close()

    def send(self, kind, **fields):
        self.seq += 1
        return self.app.command(dict(type=kind, clientId='test-client', seq=self.seq, **fields))

    def arm(self):
        self.send('arm')
        end = time.monotonic() + 1
        while self.app.state()['robot']['state'] != 'ARMED' and time.monotonic() < end:
            time.sleep(.01)
        self.assertEqual(self.app.state()['robot']['state'], 'ARMED')

    def test_drive_deadman_heartbeat_does_not_renew(self):
        self.arm()
        self.send('drive', left=200, right=-200)
        time.sleep(.06)
        self.assertEqual(self.app.state()['robot']['left'], 200)
        for _ in range(5):
            self.send('heartbeat')
            time.sleep(.09)
        state = self.app.state()['robot']
        self.assertEqual((state['state'], state['left'], state['right']), ('DISARMED', 0, 0))

    def test_zero_clears_drive_without_disarming(self):
        self.arm()
        self.send('drive', left=120, right=120)
        self.send('drive', left=0, right=0)
        time.sleep(.4)
        self.assertEqual(self.app.state()['robot']['state'], 'ARMED')

    def test_stop_invalidates_queued_arm(self):
        # Queue an arm at the exact same generation as a preceding controller.
        with self.app.lock:
            pending = Future()
            self.app.pending.append(({'type': 'arm'}, self.app.generation, 'test-client', pending))
            self.send('stop')
        with self.assertRaises(ApiError):
            pending.result(timeout=.5)
        self.assertEqual(self.app.state()['robot']['state'], 'DISARMED')

    def test_sequence_rejects_reordered_drive_after_stop(self):
        self.arm()
        self.send('stop')
        with self.assertRaisesRegex(ApiError, 'Stale'):
            self.app.command(dict(type='drive', clientId='test-client', seq=self.seq-1, left=200, right=200))
        self.assertEqual(self.app.state()['robot']['left'], 0)

    def test_control_lease_expires_independently(self):
        with self.app.lock:
            self.app.lease_deadline = clock_ms() - 1
        time.sleep(.04)
        self.assertIsNone(self.app.state()['controller']['clientId'])
        with self.assertRaisesRegex(ApiError, 'Acquire'):
            self.send('arm')

    def test_other_browser_stop_revokes_ownership(self):
        self.app.command(dict(type='stop',clientId='observer',seq=1))
        self.assertIsNone(self.app.state()['controller']['clientId'])
        with self.assertRaisesRegex(ApiError, 'Acquire'):
            self.send('arm')
        self.send('claim')
        self.send('arm')
        self.assertEqual(self.app.state()['robot']['state'], 'ARMING')

    def test_invalid_payloads_and_competing_owner(self):
        for fields in ({'left': True, 'right': 0}, {'left': 351, 'right': 0}, {'left': float('nan'), 'right': 0}):
            with self.assertRaises(ApiError):
                self.send('drive', **fields)
        with self.assertRaises(ApiError):
            self.app.command(dict(type='claim', clientId='competitor', seq=1))
        self.send('rgb', index=-1, color=0x123456)
        self.assertEqual(self.app.state()['robot']['rgb'], [0x123456]*6)

    def test_emergency_stop_latches_and_clear_never_rearms(self):
        self.send('estop')
        self.assertEqual(self.app.state()['robot']['state'], 'FAULTED')
        self.send('clearFault')
        self.assertEqual(self.app.state()['robot']['state'], 'DISARMED')

    def test_heartbeat_cannot_overtake_emergency_stop(self):
        self.app.command(dict(type='heartbeat', clientId='test-client', seq=100))
        self.app.command(dict(type='estop', clientId='test-client', seq=2))
        self.assertEqual(self.app.state()['robot']['state'], 'FAULTED')
        self.assertEqual(self.app.sequences['test-client'], 100)
        with self.assertRaisesRegex(ApiError, 'Stale'):
            self.app.command(dict(type='clearFault', clientId='test-client', seq=99))

    def test_reordered_stop_and_disarm_always_win(self):
        self.app.command(dict(type='heartbeat', clientId='test-client', seq=100))
        for kind in ('stop', 'disarm'):
            self.app.command(dict(type=kind, clientId='test-client', seq=1))
            self.assertEqual(self.app.state()['robot']['state'], 'DISARMED')
        self.assertEqual(self.app.sequences['test-client'], 100)

    def test_reordered_owner_release_revokes_control(self):
        self.send('arm')
        self.app.command(dict(type='heartbeat', clientId='test-client', seq=100))
        self.app.command(dict(type='release', clientId='test-client', seq=3))
        self.assertEqual(self.app.state()['robot']['state'], 'DISARMED')
        self.assertIsNone(self.app.state()['controller']['clientId'])
        self.assertEqual(self.app.sequences['test-client'], 100)
        with self.assertRaisesRegex(ApiError, 'Acquire'):
            self.app.command(dict(type='release', clientId='observer', seq=1))

    def test_camera_latest_frame_and_still_resume(self):
        self.send('cameraStart')
        end=time.monotonic()+1
        while self.app.camera.get_frame(-1) is None and time.monotonic()<end:
            time.sleep(.01)
        first=self.app.camera.get_frame(-1)
        self.assertTrue(first.data.startswith(b'\xff\xd8'))
        self.assertEqual(first.basis, 'server-receipt')
        self.assertGreaterEqual(clock_ms(), first.captured_ms)
        self.assertIsNone(self.app.camera.get_frame(10**12))
        photo=self.send('photo')['photo']
        self.assertTrue(self.app.camera.get_photo(photo.rsplit('/',1)[1]).startswith(b'\xff\xd8'))
        self.assertEqual(self.app.camera.snapshot()['state'], 'streaming')
        self.send('cameraStop')
        self.assertIsNone(self.app.camera.get_frame(-1))


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.app=Controller()
        self.directory=tempfile.TemporaryDirectory()
        Path(self.directory.name, 'index.html').write_text('Keel test UI')
        self.server=DemoServer(('127.0.0.1',0),self.app,'test-token-123456789',self.directory.name)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.app.close()
        self.directory.cleanup()

    def request(self, method, path, payload=None, auth=True, **headers):
        c=http.client.HTTPConnection(*self.server.server_address,timeout=3)
        if auth:
            headers['Authorization']='Bearer test-token-123456789'
        body=json.dumps(payload) if payload is not None else None
        if body is not None:
            headers['Content-Type']='application/json'
        c.request(method,path,body,headers)
        response=c.getresponse()
        result=(response.status,dict(response.getheaders()),response.read())
        c.close()
        return result

    def test_auth_static_and_traversal(self):
        self.assertEqual(self.request('GET','/api/state',auth=False)[0],401)
        self.assertEqual(self.request('GET','/',auth=False)[2],b'Keel test UI')
        self.assertEqual(self.request('GET','/%2e%2e/keel_demo.py')[0],404)
        state=json.loads(self.request('GET','/api/state')[2])
        self.assertEqual(state['language'],'Python')
        self.assertEqual(state['mode'],'mock')

    def test_cross_origin_and_nan(self):
        command=dict(type='claim',clientId='http-client',seq=1)
        self.assertEqual(self.request('POST','/api/command',command,Origin='https://evil.example')[0],403)
        self.assertEqual(self.request('POST','/api/command',command,**{'Sec-Fetch-Site':'cross-site'})[0],403)
        self.assertEqual(self.request('POST','/api/command',dict(command,seq=float('nan')))[0],400)
        self.assertEqual(self.request('POST','/api/command',command)[0],200)

    def test_ping_frame_and_photo_auth(self):
        ping=json.loads(self.request('GET','/api/ping')[2])
        self.assertLessEqual(ping['serverReceiveMs'],ping['serverSendMs'])
        self.request('POST','/api/command',dict(type='claim',clientId='http-client',seq=1))
        self.request('POST','/api/command',dict(type='cameraStart',clientId='http-client',seq=2))
        time.sleep(.08)
        status,headers,data=self.request('GET','/api/camera/frame?after=-1')
        self.assertEqual(status,200)
        self.assertEqual(headers['X-Timestamp-Basis'],'server-receipt')
        self.assertTrue(data.startswith(b'\xff\xd8'))
        self.assertEqual(self.request('GET','/api/camera/frame?after=999999')[0],204)
        photo=json.loads(self.request('POST','/api/command',dict(type='photo',clientId='http-client',seq=3))[2])['photo']
        self.assertEqual(self.request('GET',photo,auth=False)[0],401)
        self.assertEqual(self.request('GET',photo)[0],200)

    def test_accepted_sockets_disable_nagle(self):
        # Inspect the real server-side sockets after the production setup path.
        # A class-attribute assertion would not prove that the socket was configured.
        observed=[]

        class SocketProbe(Handler):
            def setup(self):
                super().setup()
                observed.append(self.connection.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY))

        self.server.RequestHandlerClass=SocketProbe
        for _ in range(2):
            self.assertEqual(self.request('GET','/api/ping')[0],200)
        self.assertEqual(observed,[1,1])

    def test_keepalive_json_large_body_head_and_empty_response_framing(self):
        # Exercise one persistent connection: fresh connections can hide the
        # small-response latency issue and cannot catch inter-response framing bugs.
        payload=bytes(range(256))*2048+b'Keel payload end'
        Path(self.directory.name,'payload.bin').write_bytes(payload)
        c=http.client.HTTPConnection(*self.server.server_address,timeout=3)
        headers={'Authorization':'Bearer test-token-123456789'}
        try:
            c.connect()
            connection=c.sock

            def exchange(method,path):
                c.request(method,path,headers=headers)
                response=c.getresponse()
                status=response.status
                response_headers=dict(response.getheaders())
                body=response.read()
                self.assertFalse(response.will_close)
                self.assertIs(c.sock,connection)
                if method!='HEAD':
                    self.assertEqual(int(response_headers['Content-Length']),len(body))
                return status,response_headers,body

            for _ in range(3):
                status,_,body=exchange('GET','/api/ping')
                self.assertEqual(status,200)
                ping=json.loads(body)
                self.assertLessEqual(ping['serverReceiveMs'],ping['serverSendMs'])

            status,_,body=exchange('GET','/payload.bin')
            self.assertEqual(status,200)
            self.assertEqual(body,payload)
            status,response_headers,body=exchange('HEAD','/payload.bin')
            self.assertEqual(status,200)
            self.assertEqual(int(response_headers['Content-Length']),len(payload))
            self.assertEqual(body,b'')
            status,_,body=exchange('GET','/api/camera/frame?after=0')
            self.assertEqual(status,204)
            self.assertEqual(body,b'')
            status,_,body=exchange('GET','/api/state')
            self.assertEqual(status,200)
            self.assertEqual(json.loads(body)['language'],'Python')
        finally:
            c.close()


if __name__ == '__main__':
    unittest.main()
