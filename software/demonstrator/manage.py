#!/usr/bin/env python3
"""Start the two demo implementations; only the selected one owns real hardware."""
from pathlib import Path
import argparse
import json
import os
import secrets
import signal
import subprocess
import sys
import time
import urllib.request

HERE = Path(__file__).resolve().parent
RUN = HERE / '.run'

def token_file():
    RUN.mkdir(mode=0o700, exist_ok=True)
    path = RUN / 'access-token'
    if not path.exists():
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(secrets.token_urlsafe(32) + '\n')
    return path

def owned_pid(language):
    path = RUN / (language + '.pid')
    try:
        pid = int(path.read_text())
        cmdline = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        expected = HERE / ('python/keel_demo.py' if language == 'python' else 'java/build/keel-demo.jar')
        return pid if str(expected).encode() in cmdline else None
    except (OSError, ValueError):
        return None

def stop(language):
    pid = owned_pid(language)
    if pid:
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 12
        while owned_pid(language) and time.monotonic() < deadline:
            time.sleep(.1)
        if owned_pid(language):
            raise RuntimeError(f'{language} did not stop cleanly; inspect {RUN/language}.log')
    (RUN / (language + '.pid')).unlink(missing_ok=True)

def start(language, mode):
    common = ['--mode', mode, '--host', '0.0.0.0', '--port', '8787' if language == 'python' else '8788',
              '--web-root', str(HERE/'web/dist'), '--token-file', str(token_file())]
    if mode == 'hardware':
        common.append('--allow-motion')
    if language == 'python':
        command = [sys.executable, str(HERE/'python/keel_demo.py'), *common]
    else:
        command = ['java', '--enable-native-access=ALL-UNNAMED', '-Djava.awt.headless=true',
                   '-XX:+UseZGC', '-jar', str(HERE/'java/build/keel-demo.jar'), *common]
    with (RUN/(language+'.log')).open('ab', buffering=0) as log:
        process = subprocess.Popen(command, cwd=HERE, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    (RUN/(language+'.pid')).write_text(str(process.pid))
    deadline = time.monotonic()+12
    port = 8787 if language == 'python' else 8788
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'{language} exited; inspect {RUN/(language+".log")}')
        try:
            request = urllib.request.Request(f'http://127.0.0.1:{port}/api/state',
                         headers={'Authorization':'Bearer '+token_file().read_text().strip()})
            with urllib.request.urlopen(request, timeout=1) as response:
                state = json.load(response)
            print(f'{language}: {state["mode"]}, {state["robot"]["state"]}, port {port}')
            return
        except (OSError, ValueError):
            time.sleep(.2)
    raise RuntimeError(f'{language} did not become ready; inspect its log')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['use', 'mock', 'stop', 'status', 'token'])
    parser.add_argument('language', nargs='?', choices=['python','java'])
    args = parser.parse_args()
    if args.action == 'token':
        print(token_file().read_text().strip()); return
    if args.action == 'status':
        for language in ('python','java'):
            print(language + ': ' + ('running (PID '+str(owned_pid(language))+')' if owned_pid(language) else 'stopped'))
        return
    if args.action == 'use' and args.language is None:
        parser.error('use requires python or java')
    if sys.platform != 'linux':
        parser.error('This process manager runs on the Pi/Linux; launch the individual servers directly elsewhere.')
    if not (HERE/'web/dist/index.html').is_file() and args.action != 'stop':
        parser.error('Build the browser assets before starting: npm --prefix web ci && npm --prefix web run build')
    token_file()
    for language in ('python','java'):
        stop(language)
    if args.action == 'stop':
        print('Both demonstrators stopped.'); return
    try:
        selected = args.language if args.action == 'use' else None
        # Open the real device first; report a failure instead of substituting mock.
        order = [selected, 'java' if selected == 'python' else 'python'] if selected else ['python','java']
        for language in order:
            start(language, 'hardware' if language == selected else 'mock')
    except Exception:
        for language in ('python','java'):
            stop(language)
        raise
    print('Access token: run ./demo token (or python3 software/demonstrator/manage.py token).')

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
