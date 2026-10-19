#!/usr/bin/env python3
"""Run the host installer against small synthetic packages and interrupted installs."""
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
import zipfile

INSTALLER = Path(__file__).with_name('install-app.py')
MANIFEST = '''<Package><Identity Name="WineUwp.Test" Publisher="CN=WineUwp" Version="1.0.0.0" ProcessorArchitecture="x64"/>
<Properties><DisplayName>Installer test</DisplayName></Properties>
<Applications><Application Id="App" Executable="test.exe"/></Applications></Package>'''


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wine-uwp-staging-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.data = self.root / 'data'
        self.stage = self.data / 'stage'
        self.stage.mkdir(parents=True)
        self.package = self.root / 'test.msix'
        self.make_package()

    def make_package(self, bad=False):
        with zipfile.ZipFile(self.package, 'w') as package:
            package.writestr('AppxManifest.xml', MANIFEST)
            package.writestr('../outside' if bad else 'test.exe', b'test payload')

    def install(self):
        return subprocess.run([sys.executable, str(INSTALLER), str(self.package), str(self.data),
            '--windows', str(self.root / 'windows'), '--stage', 'test'], capture_output=True, text=True, timeout=10)

    def test_stale_file_instead_of_staging_directory(self):
        (self.stage / 'test.new').write_bytes(b'stale helper DLL')
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.stage / 'test/test.exe').read_bytes(), b'test payload')

    def test_failure_does_not_delete_another_install(self):
        other = self.stage / 'other.new'
        other.mkdir()
        (other / 'keep').write_text('another active download')
        self.make_package(bad=True)
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((other / 'keep').exists(), result.stdout + result.stderr)
        self.assertFalse((self.stage / 'test.new').exists())

    def test_existing_install_survives_bad_package(self):
        final = self.stage / 'test'
        final.mkdir()
        (final / 'keep').write_text('installed game')
        self.make_package(bad=True)
        self.assertNotEqual(self.install().returncode, 0)
        self.assertEqual((final / 'keep').read_text(), 'installed game')

    def test_stale_symlink_does_not_remove_its_target(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'keep').write_text('untouched')
        (self.stage / 'test.new').symlink_to(outside, target_is_directory=True)
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((outside / 'keep').read_text(), 'untouched')

    def test_duplicate_worker_does_not_touch_staging(self):
        locks = self.stage / '.install-locks'
        locks.mkdir()
        work = self.stage / 'test.new'
        work.mkdir()
        (work / 'keep').write_text('active worker')
        with (locks / 'test.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.install()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('already in progress', result.stdout + result.stderr)
        self.assertEqual((work / 'keep').read_text(), 'active worker')

    def test_interrupted_publication_restores_previous_install_on_failure(self):
        backup = self.stage / 'test.old'
        backup.mkdir()
        (backup / 'keep').write_text('previous install')
        self.make_package(bad=True)
        self.assertNotEqual(self.install().returncode, 0)
        self.assertEqual((self.stage / 'test/keep').read_text(), 'previous install')

    def test_info_only_still_works(self):
        result = subprocess.run([sys.executable, str(INSTALLER), str(self.package), '--info'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)['name'], 'WineUwp.Test')

    def test_install_reports_child_pid_and_sigterm_stops_before_publication(self):
        package_bytes = self.package.read_bytes()
        get_started = threading.Event()
        release_response = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def do_HEAD(self):
                self.send_response(200)
                self.send_header('Accept-Ranges', 'bytes')
                self.send_header('Content-Length', str(len(package_bytes)))
                self.end_headers()

            def do_GET(self):
                requested = self.headers.get('Range', 'bytes=0-')
                start, end = requested.partition('=')[2].split('-', 1)
                start = int(start)
                end = int(end) if end else len(package_bytes) - 1
                body = package_bytes[start:end + 1]
                get_started.set()
                release_response.wait(5)
                try:
                    self.send_response(206)
                    self.send_header('Accept-Ranges', 'bytes')
                    self.send_header('Content-Length', str(len(body)))
                    self.send_header('Content-Range', f'bytes {start}-{end}/{len(package_bytes)}')
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        url = f'http://127.0.0.1:{server.server_port}/test.msix'
        proc = subprocess.Popen([sys.executable, str(INSTALLER), url, str(self.data),
                                 '--windows', str(self.root / 'windows'), '--stage', 'test'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        try:
            pid_line = proc.stdout.readline()
            self.assertEqual(pid_line, f'PID {proc.pid}\n')
            self.assertTrue(get_started.wait(5), 'installer did not reach the controlled local download')
            proc.send_signal(signal.SIGTERM)
            self.assertEqual(proc.wait(timeout=3), 143)
            self.assertEqual(proc.poll(), 143, 'installer process should be reaped')
            self.assertFalse((self.stage / 'test').exists())
            self.assertFalse((self.stage / 'test.new').exists())
            self.assertEqual(list(self.data.glob('wine-uwp-install-*')), [])
        finally:
            release_response.set()
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=3)
            proc.stdout.close()
            proc.stderr.close()
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=3)

    def test_selected_logo_is_staged_before_bulk_extraction(self):
        spec = importlib.util.spec_from_file_location('installer', INSTALLER)
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        logo = b'fake-png-payload'
        with zipfile.ZipFile(self.package, 'a') as package:
            package.writestr('Assets/Logo.png', logo)
        dest = self.stage / 'test.new'
        dest.mkdir()
        with zipfile.ZipFile(self.package) as package:
            icon_path = installer.stage_package_logo(package, 'Assets/Logo.png', str(dest))
        self.assertEqual(Path(icon_path).read_bytes(), logo)
        self.assertFalse((dest / 'test.exe').exists())

    def test_failed_publish_restores_installed_game(self):
        spec = importlib.util.spec_from_file_location('installer', INSTALLER)
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        final = self.stage / 'test'
        final.mkdir()
        (final / 'keep').write_text('installed game')
        rename = installer.os.rename
        def fail_publish(source, target):
            if source == str(final) + '.new':
                raise OSError('injected publication failure')
            return rename(source, target)
        with self.assertRaisesRegex(OSError, 'publication failure'):
            with installer.staging_directory(str(final)) as dest:
                (Path(dest) / 'test.exe').write_text('new game')
                with mock.patch.object(installer.os, 'rename', side_effect=fail_publish):
                    installer.publish_stage(dest, str(final))
        self.assertEqual((final / 'keep').read_text(), 'installed game')
        self.assertFalse((self.stage / 'test.new').exists())


if __name__ == '__main__':
    unittest.main()
