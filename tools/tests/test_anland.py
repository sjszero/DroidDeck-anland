import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


class AnlandServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get('CC', 'cc'))
        if compiler is None or shutil.which('pkg-config') is None:
            raise unittest.SkipTest('A C compiler and libwayland-server development files are required')
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'wayland-server'], capture_output=True, text=True)
        if flags.returncode:
            raise unittest.SkipTest('libwayland-server development files are required')
        cls.temp = tempfile.TemporaryDirectory(prefix='droiddeck-anland-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'anland-test'
        upstream = REPO / 'tools/anland/upstream'
        sources = REPO / 'app/src/main/cpp/waylandcomp/src'
        build = subprocess.run([
            compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(sources), '-I', str(upstream / 'common'),
            '-I', str(upstream / 'libdisplay_producer'),
            str(REPO / 'tools/tests/anland_server_harness.c'), str(sources / 'anland_server.c'),
            str(upstream / 'libdisplay_producer/display_producer.c'),
            str(upstream / 'common/socket_utils.c'),
            *shlex.split(flags.stdout), '-o', str(cls.binary),
        ], capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)

    def check_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'PASS')

    def test_real_producer_and_release_backpressure(self):
        self.check_case('public')

    def test_fragmented_handshake_and_clipboard(self):
        self.check_case('fragmented')

    def test_bidirectional_input_clipboard_and_refresh(self):
        self.check_case('services')

    def test_resize_reconnect_rejects_old_session(self):
        self.check_case('reconnect')

    def test_full_input_queue_disconnects_without_blocking(self):
        self.check_case('backpressure')

    def test_invalid_wire_records_disconnect_without_fd_leaks(self):
        self.check_case('malformed')


class AnlandBridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get('CC', 'cc'))
        if compiler is None or shutil.which('pkg-config') is None:
            raise unittest.SkipTest('A C compiler and libwayland-server development files are required')
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'wayland-server'], capture_output=True, text=True)
        if flags.returncode:
            raise unittest.SkipTest('libwayland-server development files are required')
        cls.temp = tempfile.TemporaryDirectory(prefix='droiddeck-anland-bridge-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'bridge-test'
        upstream = REPO / 'tools/anland/upstream'
        sources = REPO / 'app/src/main/cpp/waylandcomp/src'
        build = subprocess.run([
            compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-I', str(REPO / 'tools/tests/android_stubs'),
            '-I', str(sources), '-I', str(upstream / 'common'),
            '-I', str(upstream / 'libdisplay_producer'),
            str(REPO / 'tools/tests/anland_bridge_harness.c'),
            str(sources / 'anland_bridge.c'), str(sources / 'anland_server.c'),
            str(upstream / 'libdisplay_producer/display_producer.c'),
            str(upstream / 'common/socket_utils.c'),
            *shlex.split(flags.stdout), '-ldl', '-lm', '-pthread', '-o', str(cls.binary),
        ], capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)

    def check_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'PASS')

    def test_android_release_fences_and_old_pool_retirement(self):
        self.check_case('frames')

    def test_input_mapping_and_pointer_capture_reset(self):
        self.check_case('input')

    def test_wayland_ahb_release_keeps_its_original_owner(self):
        self.check_case('routing')

    def test_failed_or_missing_release_proof_quarantines_targets(self):
        self.check_case('failed-release')

    def test_unknown_compressed_or_truncated_layouts_are_rejected(self):
        self.check_case('layouts')


if __name__ == '__main__':
    unittest.main()
