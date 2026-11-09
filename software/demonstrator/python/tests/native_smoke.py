#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Explicit on-Pi smoke test. Never arms or drives; briefly lights LEDs.

Run only with --allow-camera-and-leds, and only when no other app owns the board.
Uses a temporary server on loopback port 18787 and always terminates it.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import threading
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--allow-camera-and-leds',action='store_true')
    parser.add_argument('--server',type=Path,default=Path(__file__).resolve().parents[1]/'keel_demo.py')
    args=parser.parse_args()
    if not args.allow_camera_and_leds:
        parser.error('explicit --allow-camera-and-leds required')
    token=secrets.token_urlsafe(24)
    sequence=0
    seq_lock=threading.Lock()
    done=threading.Event()
    log_path=args.server.parent/'native-smoke.log'
    def request(method,path,body=None,authenticated=True):
        conn=http.client.HTTPConnection('127.0.0.1',18787,timeout=15)
        headers={'Authorization':'Bearer '+token} if authenticated else {}
        if body is not None:
            body=json.dumps(body)
            headers['Content-Type']='application/json'
        conn.request(method,path,body,headers)
        r=conn.getresponse()
        result=(r.status,dict(r.getheaders()),r.read())
        conn.close()
        return result
    def command(kind,**fields):
        nonlocal sequence
        with seq_lock:
            sequence+=1
            value=sequence
        status,_,body=request('POST','/api/command',dict(type=kind,clientId='native-smoke',seq=value,**fields))
        if status!=200:
            raise RuntimeError(f'{kind}: HTTP {status}: {body.decode()}')
        return json.loads(body)
    def heartbeat():
        while not done.wait(.25):
            try:
                command('heartbeat')
            except Exception as error:
                print('Heartbeat:',error)
    with log_path.open('w') as log:
        process=subprocess.Popen([sys.executable,str(args.server),'--mode','hardware','--port','18787'],
            env=dict(os.environ,KEEL_DEMO_TOKEN=token),stdout=log,stderr=log)
        thread=None
        try:
            deadline=time.monotonic()+15
            while True:
                if process.poll() is not None:
                    raise RuntimeError('Server exited: '+log_path.read_text())
                try:
                    status,_,body=request('GET','/api/state')
                    if status==200:
                        state=json.loads(body)
                        break
                except OSError:
                    pass
                if time.monotonic()>deadline:
                    raise RuntimeError('Startup timed out')
                time.sleep(.15)
            assert state['mode']=='hardware' and not state['motionAllowed']
            assert state['robot']['state']=='DISARMED',state['robot']
            assert request('GET','/api/state',authenticated=False)[0]==401
            print('Native board:',json.dumps(state['robot']))
            command('claim')
            thread=threading.Thread(target=heartbeat,daemon=True)
            thread.start()
            command('rgb',index=-1,color=0x030405)
            command('buttonLeds',mask=1)
            time.sleep(.05)
            state=json.loads(request('GET','/api/state')[2])
            assert state['robot']['rgb']==[0x030405]*6,state['robot']
            assert state['robot']['buttonLeds']==1,state['robot']
            command('rgb',index=-1,color=0)
            command('buttonLeds',mask=0)
            print('LED round-trip: PASS; all LEDs off')
            command('cameraStart')
            deadline=time.monotonic()+5
            while True:
                status,headers,data=request('GET','/api/camera/frame?after=-1')
                if status==200:
                    break
                assert status==204,(status,data)
                if time.monotonic()>deadline:
                    raise RuntimeError('No native camera frame')
                time.sleep(.1)
            assert data.startswith(b'\xff\xd8')
            assert headers['X-Timestamp-Basis']=='sensor',headers
            age=float(headers['X-Capture-Age-Ms'])
            assert 0 <= age < 3000,age
            print('Native JPEG:',len(data),'bytes; frame age',round(age,2),'ms; basis',headers['X-Timestamp-Basis'])
            old_sequence=int(headers['X-Sequence'])
            photo=command('photo')['photo']
            status,headers,data=request('GET',photo)
            assert status==200 and data.startswith(b'\xff\xd8')
            print('Native still:',len(data),'bytes; stream resumed')
            deadline=time.monotonic()+4
            while True:
                status,headers,data=request('GET',f'/api/camera/frame?after={old_sequence}')
                if status==200:
                    break
                if time.monotonic()>deadline:
                    raise RuntimeError('Stream did not resume after still')
                time.sleep(.1)
            state=json.loads(request('GET','/api/state')[2])
            assert not state['camera']['autofocus'],state['camera']
            assert state['robot']['state']=='DISARMED',state['robot']
            print('Camera stats:',json.dumps(state['camera']))
            print('Control loop:',json.dumps(state['loop']))
            command('cameraStop')
            assert request('GET','/api/camera/frame?after=-1')[0]==204
            command('release')
            print('NATIVE_SMOKE_PASS (no arm or drive commands)')
        finally:
            done.set()
            if thread:
                thread.join(timeout=2)
            for kind,fields in [('rgb',dict(index=-1,color=0)),('buttonLeds',dict(mask=0)),('cameraStop',{})]:
                try:
                    command(kind,**fields)
                except Exception:
                    pass
            process.terminate()
            try:
                process.wait(timeout=12)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            print('Temporary server stopped; log:',log_path)


if __name__=='__main__':
    main()
