"""The online play's native tests (gpu/CMakeLists.txt: net-tunnel-test, net-ping-test,
http-ryunet-test, launcher-online-test), run when they are built (Windows: ninja -C out/gpu
<target>; Linux: build.sh --test builds and runs the first three). Each talks only to 127.0.0.1."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

BUILD = ROOT / 'out' / 'gpu'
MSYS2_BIN = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'


class OnlineNativeTests(unittest.TestCase):
    def run_test(self, name):
        exe = BUILD / (name + ('.exe' if sys.platform == 'win32' else ''))
        if not exe.is_file():
            self.skipTest(f'{exe.name} is not built')
        env = dict(os.environ)
        if sys.platform == 'win32' and MSYS2_BIN.is_dir():  # libc++ and the other CLANG64 DLLs
            env['PATH'] = f'{MSYS2_BIN}{os.pathsep}{env.get("PATH", "")}'
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([str(exe)], cwd=tmp, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, errors='replace', timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:])

    def test_net_tunnel(self):
        self.run_test('net-tunnel-test')

    def test_net_ping(self):
        self.run_test('net-ping-test')

    def test_http_ryunet(self):
        self.run_test('http-ryunet-test')

    def test_launcher_online(self):
        self.run_test('launcher-online-test')


if __name__ == '__main__':
    unittest.main()
