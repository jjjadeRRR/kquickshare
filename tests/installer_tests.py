#!/usr/bin/env python3
"""Exercise the installer CLI and version guard without touching the host desktop."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallerTests(unittest.TestCase):
    def test_dependency_plans(self):
        for distro, manager, required in [
            ('arch', 'pacman', 'kstatusnotifieritem'),
            ('debian', 'apt-get', 'libkf6statusnotifieritem-dev'),
            ('ubuntu', 'apt-get', 'qt6-wayland'),
            ('fedora', 'dnf', 'kf6-kstatusnotifieritem-devel'),
            ('opensuse-tumbleweed', 'zypper', 'kf6-kstatusnotifieritem-devel'),
        ]:
            with self.subTest(distro=distro):
                result = subprocess.run([str(ROOT / 'install.sh'), '--distro', distro, '--print-deps'],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(manager, result.stdout)
                self.assertIn(required, result.stdout)
                self.assertNotIn('Building KQuickShare', result.stdout)

    def test_unknown_distribution_has_actionable_error(self):
        result = subprocess.run([str(ROOT / 'install.sh'), '--distro', 'unknown', '--print-deps'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--no-deps', result.stderr)

    def test_clipboard_overlay_accepts_supported_series(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            tools = base / 'bin'
            tools.mkdir()
            plugin = base / 'qml/org/kde/plasma/private/clipboard/libklipperplugin.so'
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            for name, body in {
                'plasmashell': 'echo "plasmashell 6.8.1"',
                'qtpaths6': f'echo "{base}/qml"',
                'systemctl': 'exit 0',
            }.items():
                tool = tools / name
                tool.write_text('#!/bin/sh\n' + body + '\n')
                tool.chmod(0o755)
            env = {**os.environ, 'PATH': str(tools) + ':' + os.environ['PATH'],
                   'XDG_DATA_HOME': str(base / 'data'), 'XDG_CONFIG_HOME': str(base / 'config')}
            result = subprocess.run([str(ROOT / 'integration/install-clipboard.sh')],
                                    env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            module = base / 'data/kquickshare/qml/org/kde/plasma/private/clipboard'
            self.assertTrue((module / 'DelegateToolButtons.qml').is_file())
            self.assertEqual((module / 'libklipperplugin.so').resolve(), plugin)
            self.assertTrue((base / 'config/systemd/user/plasma-plasmashell.service.d/kquickshare-clipboard.conf').is_file())

    def test_clipboard_overlay_refuses_other_plasma_versions_before_writing(self):
        for version in ('5.27.12', '6.2.5', '6.7.0', '6.9.0'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp:
                base = Path(temp)
                tools = base / 'bin'
                tools.mkdir()
                shell = tools / 'plasmashell'
                shell.write_text(f'#!/bin/sh\necho "plasmashell {version}"\n')
                shell.chmod(0o755)
                env = {**os.environ, 'PATH': str(tools) + ':' + os.environ['PATH'],
                       'XDG_DATA_HOME': str(base / 'data'), 'XDG_CONFIG_HOME': str(base / 'config')}
                result = subprocess.run([str(ROOT / 'integration/install-clipboard.sh')],
                                        env=env, text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('6.8', result.stderr)
                self.assertFalse((base / 'data').exists())
                self.assertFalse((base / 'config').exists())


if __name__ == '__main__':
    unittest.main()
