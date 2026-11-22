"""Exercise add-ons (DLC) through XPackage and XStore against a local fake bridge."""
import argparse
import http.server
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

SECRET = 'xtux-test-secret'
OWNED = '11111111-2222-3333-4444-555555555555'
UNOWNED = 'AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE'
NEW = '99999999-8888-7777-6666-555555555555'

FIRST = ('gen1\n'
         f'{OWNED}\t9PCR7TSM1S96\t1.2.3.4\tZ:\\games\\Rally\\Content\tRally Pack\tStudio\n'
         f'{UNOWNED}\t9P5H6T4ZVR8R\t1.0.0.0\tZ:\\games\\Night\\Content\tNight Pack\tStudio\n'
         # Malformed lines are left out.
         'not-an-id\t9P5H6T4ZVR8R\t1.0.0.0\tZ:\\x\tBad\tStudio\n'
         f'{OWNED}\tshort\n')
SECOND = FIRST.replace('gen1', 'gen2') + f'{NEW}\t9NBLGGH4R315\t1.0.0.0\tZ:\\games\\New\\Content\tNew Pack\tStudio\n'

CONFIG = '''<Game configVersion="1">
  <Identity Name="XTux.AddonTest" Publisher="CN=XTux Tests" Version="2.0.0.7" />
  <StoreId>9NNX1VVR3KNQ</StoreId>
  <ShellVisuals DefaultDisplayName="Addon Test" PublisherDisplayName="XTux Tests" />
</Game>
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', default='', help='Wine launcher; empty runs the binary directly (Windows)')
    parser.add_argument('--binary', required=True)
    parser.add_argument('--dll')
    args = parser.parse_args()
    wine = Path(args.wine).resolve() if args.wine else None
    server = None
    if wine:
        server = Path(args.wine).absolute().parent / 'server/wineserver'
        if not server.exists():
            found = shutil.which('wineserver', path=str(wine.parent)) or shutil.which('wineserver')
            server = Path(found) if found else None
    binary = Path(args.binary).resolve()
    dll = Path(args.dll).resolve() if args.dll else binary.parent / 'xgameruntime.dll'
    requests = []
    lock = threading.Lock()

    class Bridge(http.server.BaseHTTPRequestHandler):
        def answer(self, status, body):
            data = body.encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if self.headers.get('X-Xodus-Secret') != SECRET:
                self.send_error(403)
                return
            count = int(self.headers.get('Content-Length', '0'))
            body = self.rfile.read(count).decode('utf-8')
            with lock:
                requests.append((self.path, body))
            if self.path == '/packages':
                self.answer(200, FIRST)
            elif self.path == '/packages/wait':
                if body.strip() == 'gen1':
                    # The add-on finishes installing a moment later.
                    time.sleep(1)
                    self.answer(200, SECOND)
                else:
                    time.sleep(2)
                    self.answer(200, SECOND)
            elif self.path == '/store/package-license':
                if body.strip() in (OWNED, NEW):
                    self.answer(200, '1')
                else:
                    self.answer(403, 'not entitled')
            else:
                self.send_error(404)

        def log_message(self, *unused):
            pass

    with tempfile.TemporaryDirectory(prefix='xtux-addon-test-') as temp:
        work = Path(temp)
        prefix = work / 'prefix'
        prefix.mkdir()
        shutil.copy2(binary, work / binary.name)
        shutil.copy2(dll, work / 'xgameruntime.dll')
        (work / 'MicrosoftGame.config').write_text(CONFIG)
        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Bridge) as bridge:
            thread = threading.Thread(target=bridge.serve_forever, daemon=True)
            thread.start()
            env = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH='win64', WINEDEBUG='-all',
                       WINEDLLOVERRIDES='xgameruntime=n', XODUS_BRIDGE_PORT=str(bridge.server_port),
                       XODUS_BRIDGE_SECRET=SECRET, XODUS_LICENSE_VERIFIED='1',
                       XODUS_PACKAGE_ID='0123456789ABCDEF0123456789ABCDEF')
            env.pop('WINE_DLL_FILE_MAP', None)
            try:
                command = ([str(wine)] if wine else []) + [str(work / binary.name)]
                result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=120)
                print(result.stdout, end='')
                if result.returncode:
                    print(result.stderr[-3000:])
                    return result.returncode
                licenses = [body for path, body in requests if path == '/store/package-license']
                # Each add-on's license is asked only once per run.
                assert len(licenses) == len(set(licenses)), f'license asked more than once: {licenses}'
                print('Bridge verified: each license asked once.')
            finally:
                bridge.shutdown()
                thread.join(timeout=5)
                if server and server.exists():
                    subprocess.run([str(server), '-k'], env=env, capture_output=True, timeout=10)
                    subprocess.run([str(server), '-w'], env=env, capture_output=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
