"""run.py setup_online (online play on a RyuNET server, from Ryu7w7/OCTR-NitroFueled-PC): off
unless asked, a nickname of the player's own, the installation's key kept next to run.py, the
nickname claimed with the key's hash for that server, and the game's Demonware hosts answered by
the server. A local fake server; no network."""
import contextlib
import http.server
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT

sys.path.insert(0, str(ROOT))

ONLINE_VARIABLES = ('BB_ONLINE', 'BB_ONLINE_NAME', 'BB_ONLINE_SERVER', 'BB_ONLINE_HTTP_PORT', 'BB_ONLINE_KEYHASH',
                    'BB_ONLINE_INVENTORY', 'BB_ONLINE_CLOCK_OFFSET', 'BB_USER_NAME', 'BB_DATA_DIR', 'BB_NET_TRACE')


class SetupOnlineTests(unittest.TestCase):
    def setUp(self):
        import run
        self.run = run
        self.lines = []
        self.patch(run, 'say', self.lines.append)
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp, ignore_errors=True))
        self.config = self.tmp / 'bbport.ini'
        # The key's copy outside the folder goes to this test's folder, not the real %APPDATA%.
        self.patch(run, 'key_store', lambda: self.tmp / 'store')
        self.claims = []
        test = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def answer(self):
                path = self.path.split('?')[0]
                test.claims.append((self.command, path, self.headers.get('X-RyuNET-Key-Hash'),
                                    self.headers.get('X-RyuNET-Proof')))
                status = 'claimed' if path == '/ryunet/nick/claim' else 'free'
                body = json.dumps({} if path == '/ryunet/theme' else {'status': status}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            do_GET = do_POST = answer

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.port = self.server.server_address[1]

    def patch(self, owner, name, value):
        old = getattr(owner, name)
        setattr(owner, name, value)
        self.addCleanup(setattr, owner, name, old)

    def setup(self, ini, environ=None):
        self.config.write_text(ini, encoding='utf-8')
        env = {k: v for k, v in os.environ.items() if k not in ONLINE_VARIABLES}
        env.update(environ or {})
        self.run.setup_online(env, str(self.config), self.tmp)
        return env

    def test_off_by_default(self):
        env = self.setup('online_name=Tester\n')
        self.assertNotIn('BB_ONLINE', env)
        self.assertFalse((self.tmp / 'host_overrides.json').exists())
        self.assertFalse((self.tmp / 'ryunet_online.key').exists())
        self.assertEqual(self.claims, [])

    def test_shared_or_bad_nicknames_are_refused(self):
        for name in ('Player', 'guest', 'ab', 'way_too_long_nickname', 'bad name'):
            with self.assertRaises(SystemExit, msg=name), contextlib.redirect_stderr(io.StringIO()):
                self.setup(f'online=1\nonline_name={name}\nonline_server=127.0.0.1:{self.port}\n')

    def test_online_claims_the_nickname_and_routes_demonware(self):
        env = self.setup(f'online=1\nonline_name=Tester\nonline_server=127.0.0.1:{self.port}\n')
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertEqual(env['BB_USER_NAME'], 'Tester')
        self.assertEqual(env['BB_ONLINE_NAME'], 'Tester')
        self.assertEqual(env['BB_ONLINE_SERVER'], '127.0.0.1')
        self.assertEqual(env['BB_ONLINE_HTTP_PORT'], str(self.port))
        self.assertEqual(env['BB_DATA_DIR'], str(self.tmp))
        # The key stays on the PC (next to run.py and a copy outside the folder); the server only
        # ever sees the hash of its HMAC for this host.
        key = (self.tmp / 'ryunet_online.key').read_text().strip()
        self.assertEqual(len(key), 64)
        self.assertEqual((self.tmp / 'store' / 'ryunet_online.key').read_text().strip(), key)
        khash = self.run.key_hash(self.run.server_key(key, '127.0.0.1'))
        self.assertEqual(env['BB_ONLINE_KEYHASH'], khash)
        self.assertEqual([(m, p) for m, p, _, _ in self.claims],
                         [('GET', '/ryunet/theme'), ('GET', '/ryunet/nick'), ('POST', '/ryunet/nick/claim')])
        self.assertEqual(self.claims[2][2], khash)
        self.assertTrue(all(proof for _, p, _, proof in self.claims if p.startswith('/ryunet/nick')))
        self.assertNotIn(key, json.dumps(self.claims))
        overrides = json.loads((self.tmp / 'host_overrides.json').read_text())
        self.assertEqual(overrides['lavender-ps4-auth3.prod.demonware.net'], f'http://127.0.0.1:{self.port}')
        self.assertEqual(overrides['lavender-ps4-lobby.prod.demonware.net'], '127.0.0.1')
        self.assertEqual(overrides['stun.eu.demonware.net'], '127.0.0.1')
        self.assertIn('Nickname Tester: claimed on 127.0.0.1', self.lines)
        # The same installation keeps its key.
        env = self.setup(f'online=1\nonline_name=Tester\nonline_server=127.0.0.1:{self.port}\n')
        self.assertEqual((self.tmp / 'ryunet_online.key').read_text().strip(), key)

    def test_environment_overrides_the_ini(self):
        env = self.setup(f'online=1\nonline_name=Tester\nonline_server=127.0.0.1:{self.port}\n', {'BB_ONLINE': '0'})
        self.assertNotIn('BB_ONLINE', env)
        env = self.setup('online=0\n', {'BB_ONLINE': '1', 'BB_ONLINE_NAME': 'Racer_7',
                                        'BB_ONLINE_SERVER': f'127.0.0.1:{self.port}', 'BB_ONLINE_INVENTORY': 'save'})
        self.assertEqual((env['BB_ONLINE'], env['BB_ONLINE_NAME'], env['BB_ONLINE_INVENTORY']), ('1', 'Racer_7', 'save'))


if __name__ == '__main__':
    unittest.main()
