import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SESSION = ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-session'
DESKTOP = ROOT / 'tools/linuxfs/desktop/droiddeck-desktop'
KWIN = ROOT / 'tools/linuxfs/desktop/kwin_wayland_wrapper'
KEYS = ('ANLAND_SOCKET', 'ANLAND_DRM_DEVICE', 'ANLAND_DISABLE_AUDIO',
        'ANLAND_DISABLE_CAMERA', 'KWIN_RENDER_NODES', 'KWIN_COMPOSE',
        'LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER', 'MESA_LOADER_DRIVER_OVERRIDE')


class AnlandLaunchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='anland-launch-')
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.endpoint = self.path / 'anland.sock'
        self.sock = socket.socket(socket.AF_UNIX)
        self.addCleanup(self.sock.close)
        self.sock.bind(str(self.endpoint))
        self.env = {k: v for k, v in os.environ.items()
                    if k not in KEYS and not k.startswith(('BL_', 'DROIDDECK_', 'ANLAND_'))}
        self.env.update(XDG_RUNTIME_DIR=str(self.path), BL_WIDTH='1280', BL_HEIGHT='720',
                        BL_REFRESH='120', BL_MANGOAPP='0')
        self.capture = self.path / 'capture'
        self.capture.write_text('#!' + sys.executable + '\nimport json, os, sys\n'
                                'print("CAPTURE:" + json.dumps({"args": sys.argv[1:], '
                                '"env": {k: os.environ.get(k) for k in ' + repr(KEYS) + '}}))\n')
        self.capture.chmod(0o755)

    def run_script(self, script, **env):
        return subprocess.run(['bash', '-c', script], env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=5)

    def captured(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(next(line.removeprefix('CAPTURE:') for line in result.stdout.splitlines()
                               if line.startswith('CAPTURE:')))

    def gamescope(self, **env):
        source = SESSION.read_text()
        start = source.index('  width=${BL_WIDTH:-1280}')
        end = source.index('\nfi\n\nbl_displays()', start)
        block = source[start:end].replace('/usr/local/bin/droiddeck-pipewire', 'pipewire_stub')
        script = ('set -u\nmode=steam\npipewire_stub() { return 1; }\n'
                  'gamescope() { printf "%s\\n" "${TEST_HELP:-anland => host buffers}"; }\n'
                  'run_gamescope() { ' + str(self.capture) + ' "$@"; }\n' + block)
        return self.run_script(script, **env)

    def kwin(self, **env):
        helper = self.path / 'kwin-help'
        helper.write_text('#!/bin/sh\nprintf "%s\\n" "${TEST_HELP:---anland}"\n')
        helper.chmod(0o755)
        script = KWIN.read_text().replace('/usr/bin/kwin_wayland_wrapper', str(self.capture))
        script = script.replace('/usr/bin/kwin_wayland', str(helper))
        return self.run_script(script, BL_DESKTOP='1', **env)

    def test_socket_environment_alone_does_not_change_default_backend(self):
        for launcher in (self.gamescope, self.kwin):
            run = self.captured(launcher(ANLAND_SOCKET=str(self.endpoint)))
            self.assertIsNone(run['env']['ANLAND_DISABLE_AUDIO'])
            self.assertNotIn('--anland', run['args'])
            if launcher == self.gamescope:
                self.assertEqual(run['args'][:2], ['--backend', 'wayland'])
            else:
                self.assertIn('--wayland-display', run['args'])

    def test_gamescope_opt_in_keeps_host_audio_and_the_selected_render_device(self):
        run = self.captured(self.gamescope(DROIDDECK_GAMESCOPE_BACKEND='anland',
                                           ANLAND_DRM_DEVICE='/dev/dri/renderD307'))
        self.assertEqual(run['args'][:2], ['--backend', 'anland'])
        self.assertEqual(run['env']['ANLAND_SOCKET'], str(self.endpoint))
        self.assertEqual(run['env']['ANLAND_DRM_DEVICE'], '/dev/dri/renderD307')
        self.assertEqual(run['env']['ANLAND_DISABLE_AUDIO'], '1')
        self.assertEqual(run['env']['ANLAND_DISABLE_CAMERA'], '1')

    def test_kwin_opt_in_uses_host_targets_and_keeps_its_client_socket(self):
        run = self.captured(self.kwin(DROIDDECK_KWIN_BACKEND='anland',
                                     ANLAND_DRM_DEVICE='/dev/dri/renderD307'))
        self.assertIn('--anland', run['args'])
        self.assertNotIn('--wayland-display', run['args'])
        self.assertEqual(run['args'][run['args'].index('--socket') + 1], 'wayland-1')
        self.assertEqual(run['env']['ANLAND_SOCKET'], str(self.endpoint))
        self.assertEqual(run['env']['KWIN_RENDER_NODES'], '/dev/dri/renderD307')
        self.assertEqual(run['env']['ANLAND_DISABLE_AUDIO'], '1')

    def test_opt_in_explains_when_the_component_has_not_been_rebuilt(self):
        for launcher, variable in ((self.gamescope, 'DROIDDECK_GAMESCOPE_BACKEND'),
                                   (self.kwin, 'DROIDDECK_KWIN_BACKEND')):
            run = launcher(**{variable: 'anland', 'TEST_HELP': 'old component'})
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('Anland support first', run.stderr)
            self.assertNotIn('CAPTURE:', run.stdout)

    def test_opt_in_explains_when_the_host_is_absent(self):
        self.endpoint.unlink()
        for launcher, variable in ((self.gamescope, 'DROIDDECK_GAMESCOPE_BACKEND'),
                                   (self.kwin, 'DROIDDECK_KWIN_BACKEND')):
            run = launcher(**{variable: 'anland'})
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('endpoint is unavailable', run.stderr)
            self.assertNotIn('CAPTURE:', run.stdout)

    def test_desktop_cleanup_preserves_both_host_endpoints(self):
        (self.path / 'wayland-0').touch()
        (self.path / 'wayland-1').touch()
        (self.path / 'keymap.xkb').touch()
        (self.path / 'stale').touch()
        cleanup = next(line for line in DESKTOP.read_text().splitlines()
                       if line.startswith('find "$XDG_RUNTIME_DIR"'))
        result = self.run_script(cleanup)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.endpoint.is_socket())
        self.assertTrue((self.path / 'wayland-0').exists())
        self.assertTrue((self.path / 'keymap.xkb').exists())
        self.assertFalse((self.path / 'wayland-1').exists())
        self.assertFalse((self.path / 'stale').exists())

    def test_anland_desktop_enables_gpu_even_with_software_desktop_defaults(self):
        source = DESKTOP.read_text()
        start = source.index('if [ "$(cat /usr/share/droiddeck/kwin-compositor')
        end = source.index('\necho "== droiddeck-desktop: Plasma on ', start)
        block = source[start:end].replace('/usr/share/droiddeck/kwin-compositor', str(self.path / 'mode'))
        block = block.replace('/run/droiddeck/kwin', str(self.path / 'kwin'))
        for backend, expected in (('wayland', 'llvmpipe'), ('anland', 'zink')):
            result = self.run_script(block + '\n' + str(self.capture),
                                     DROIDDECK_KWIN_BACKEND=backend, GALLIUM_DRIVER='llvmpipe',
                                     LIBGL_ALWAYS_SOFTWARE='1', KWIN_COMPOSE='Q',
                                     KWIN_RENDER_NODES='/dev/dri/renderD307')
            run = self.captured(result)
            self.assertEqual(run['env']['GALLIUM_DRIVER'], expected)
            self.assertEqual(run['env']['LIBGL_ALWAYS_SOFTWARE'], None if backend == 'anland' else '1')


if __name__ == '__main__':
    unittest.main()
