"""Exercise the native token ABI using a local bridge; never requests account tokens."""
import argparse
import base64
import http.server
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', default='', help='Wine launcher; empty runs the binary directly (Windows)')
    parser.add_argument('--binary', required=True)
    parser.add_argument('--dll')
    args = parser.parse_args()
    wine = Path(args.wine).resolve() if args.wine else None
    server = None
    if wine:
        # The build-tree wine launcher is a symlink; keep its original parent for wineserver.
        server = Path(args.wine).absolute().parent / 'server/wineserver'
        if not server.exists():
            found = shutil.which('wineserver', path=str(wine.parent)) or shutil.which('wineserver')
            server = Path(found) if found else None
    binary = Path(args.binary).resolve()
    dll = Path(args.dll).resolve() if args.dll else binary.parent / 'xgameruntime.dll'
    requests = []

    class Bridge(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != '/xbl/token' or self.headers.get('X-Xodus-Secret') != 'xtux-test-secret':
                self.send_error(403)
                return
            count = int(self.headers.get('Content-Length', '0'))
            if not 0 <= count <= 65536:
                self.send_error(413)
                return
            parts = self.rfile.read(count).decode('utf-8').split('\n')
            requests.append(parts)
            if len(parts) != 6:
                self.send_error(400)
                return
            status, body = 200, 'test-token-ä-🌍\nsig-β'.encode('utf-8')
            if parts[1].endswith('/denied'):
                status, body = 403, b'denied'
            elif parts[1].endswith('/empty'):
                status, body = 204, b''
            elif parts[1].endswith('/invalid'):
                body = b'\xff\nsig'
            self.send_response(status)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *unused):
            pass

    with tempfile.TemporaryDirectory(prefix='xtux-token-test-') as temp:
        work = Path(temp)
        prefix = work / 'prefix'
        prefix.mkdir()
        shutil.copy2(binary, work / binary.name)
        shutil.copy2(dll, work / 'xgameruntime.dll')
        config = binary.parent / 'MicrosoftGame.config'
        if config.exists():
            shutil.copy2(config, work / config.name)
        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Bridge) as bridge:
            thread = threading.Thread(target=bridge.serve_forever, daemon=True)
            thread.start()
            env = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH='win64', WINEDEBUG='-all',
                       WINEDLLOVERRIDES='xgameruntime=n', XODUS_BRIDGE_PORT=str(bridge.server_port),
                       XODUS_BRIDGE_SECRET='xtux-test-secret')
            env.pop('WINE_DLL_FILE_MAP', None)
            try:
                command = ([str(wine)] if wine else []) + [str(work / binary.name)]
                result = subprocess.run(command, cwd=work, env=env,
                                        capture_output=True, text=True, timeout=90)
                print(result.stdout, end='')
                if result.returncode:
                    print(result.stderr[-2000:])
                    return result.returncode
                assert len(requests) == 5, f'Unexpected bridge request count: {len(requests)}'
                utf16 = requests[1]
                assert utf16[:3] == ['POST', 'https://example.test/utf16/ä', '1']
                assert base64.b64decode(utf16[5], validate=True) == b'a\x00b'
                print('Bridge verified UTF-8 URL conversion, force-refresh and binary request body.')
            finally:
                bridge.shutdown()
                thread.join(timeout=5)
                if server and server.exists():
                    subprocess.run([str(server), '-k'], env=env, capture_output=True, timeout=10)
                    subprocess.run([str(server), '-w'], env=env, capture_output=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
