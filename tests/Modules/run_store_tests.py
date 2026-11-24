"""Exercise the Store and in-game events against a local fake bridge."""
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
INSTALLED = '11111111-2222-3333-4444-555555555555'
SCID = '00000000-0000-0000-0000-00006d0fd16e'

PACKAGES = f'gen1\n{INSTALLED}\t9MT1H69FDCW0\t1.2.3.4\tZ:\\games\\Car\\Content\tCar\tStudio\n'


def product(store_id, kind, quantity, base, price, offer, title):
    fields = [store_id, kind, '0010', quantity, 1, base, price, 'EUR' if offer else '', 0, offer, 'token',
              'https://store-images.s-microsoft.com/image/a', 10, 20, title, 'Text']
    return '\t'.join(str(f) for f in fields) + '\n'


RALLY = product('9PCR7TSM1S96', 'Durable', 1, '19.99', '7.99', '9OFFER', 'Rally')
PRODUCTS = (RALLY + product('9P5H6T4ZVR8R', 'Durable', 0, '5.00', '5.00', '9OTHER', 'Night')
            + product('9COINS000001', 'Consumable', 2, '0.00', '0.00', '', 'Coins')
            # Lines that do not fit are left out.
            + 'short\tline\n' + product('9TOOLONGSTOREID00', 'Durable', 0, '1.00', '1.00', '', 'Bad')
            + product('9UNKNOWN0001', 'Application', 0, '1.00', '1.00', '', 'Bad'))

CONFIG = '''<Game configVersion="1">
  <Identity Name="XTux.StoreTest" Publisher="CN=XTux Tests" Version="2.0.0.7" />
  <StoreId>9NNX1VVR3KNQ</StoreId>
  <ShellVisuals DefaultDisplayName="Store Test" PublisherDisplayName="XTux Tests" />
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
            lines = body.split('\n')
            if self.path == '/packages':
                self.answer(200, PACKAGES)
            elif self.path == '/packages/wait':
                time.sleep(2)
                self.answer(200, PACKAGES)
            elif self.path == '/store/package-license':
                # Not owned: the installed add-on does not show up in the lists of what the user owns.
                self.answer(403, 'not entitled')
            elif self.path == '/store/products' and lines[0] == 'associated':
                self.answer(200, PRODUCTS)
            elif self.path == '/store/products':
                self.answer(200, ''.join(line + '\n' for line in PRODUCTS.split('\n') if line.split('\t')[0] in lines[1:] and line))
            elif self.path == '/store/consumable' and body == '9COINS000001':
                self.answer(200, '2')
            elif self.path == '/store/consumable':
                self.answer(502, 'not a consumable')
            elif self.path == '/store/consume':
                self.answer(200, '1')
            elif self.path == '/store/user-key':
                self.answer(200, lines[0] + '-key')
            elif self.path == '/xbox/event':
                self.answer(204, '')
            else:
                self.send_error(404)

        def log_message(self, *unused):
            pass

    with tempfile.TemporaryDirectory(prefix='xtux-store-test-') as temp:
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
                assert ('/store/products', 'associated\n9NNX1VVR3KNQ\n') in requests, requests
                assert ('/store/products', 'associated\n9PCR7TSM1S96\n') in requests, requests
                assert ('/store/consume', '9COINS000001\n1\n11223344-5566-7788-99aa-bbccddeeff01\n') in requests, requests
                assert ('/store/user-key', 'collections\nticket\nplayer\n') in requests, requests
                assert ('/store/user-key', 'purchase\nticket\n\n') in requests, requests
                events = [body for path, body in requests if path == '/xbox/event']
                assert events[0] == f'{SCID}\nplay\nEvent0\n{{  "Map": "A"}}\n{{"Count":1}}\n', events[0]
                assert [e.split('\n')[2] for e in events] == [f'Event{i}' for i in range(20)], events
                print('Bridge verified: Store requests, and events in order.')
            finally:
                bridge.shutdown()
                thread.join(timeout=5)
                if server and server.exists():
                    subprocess.run([str(server), '-k'], env=env, capture_output=True, timeout=10)
                    subprocess.run([str(server), '-w'], env=env, capture_output=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
