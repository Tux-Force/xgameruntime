"""Exercise the system UI (XGameUi) and the gamer picture against a local fake bridge."""
import argparse
import base64
import http.server
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

SECRET = 'xtux-test-secret'
PICTURE = bytes(range(256)) * 3 + b'x'

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
            if self.path == '/xbox/profile':
                self.answer(200, 'Pl\u00e4yer\nGamerscore: 12')
            elif self.path == '/xbox/achievements':
                self.answer(200, 'Achievements\n' + 'line\n' * 100)
            elif self.path == '/xbox/gamer-picture':
                self.answer(200, base64.b64encode(PICTURE).decode())
            else:
                self.send_error(404)

        def log_message(self, *unused):
            pass

    with tempfile.TemporaryDirectory(prefix='xtux-gameui-test-') as temp:
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
                       XODUS_XUID='2533274790395900', XODUS_GAMERTAG='Tester')
            env.pop('WINE_DLL_FILE_MAP', None)
            try:
                command = ([str(wine)] if wine else []) + [str(work / binary.name)]
                result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=120)
                print(result.stdout, end='')
                if result.returncode:
                    print(result.stderr[-3000:])
                    return result.returncode
                assert ('/xbox/profile', '2533274790395904') in requests, requests
                assert ('/xbox/achievements', '7') in requests, requests
                assert ('/xbox/gamer-picture', '2533274790395900\n208') in requests, requests
                print('Bridge verified.')
            finally:
                bridge.shutdown()
                thread.join(timeout=5)
                if server and server.exists():
                    subprocess.run([str(server), '-k'], env=env, capture_output=True, timeout=10)
                    subprocess.run([str(server), '-w'], env=env, capture_output=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
