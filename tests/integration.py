"""Exercise the compiled server through real loopback TCP sockets (stdlib only)."""
import csv
import os
from pathlib import Path
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Server:
    def __init__(self, directory, *options):
        self.log = Path(directory) / 'capture.csv'
        self.lines = []
        self.output = queue.Queue()
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
        # Windows CI/sandbox subprocesses may have no console for CTRL_BREAK_EVENT.
        # Test the same drain path with a finite runtime there; Linux exercises SIGTERM.
        duration = ['--run-for-ms', '3000'] if os.name == 'nt' else []
        self.process = subprocess.Popen([BINARY, '--port', '0', '--log', str(self.log), *duration, *options],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, creationflags=flags)
        def read_output():
            for line in self.process.stdout:
                self.lines.append(line.rstrip())
                self.output.put(line.rstrip())
            self.process.stdout.close()
        self.reader = threading.Thread(target=read_output, daemon=True)
        self.reader.start()
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try:
                    line = self.output.get(timeout=0.1)
                except queue.Empty:
                    if self.process.poll() is not None:
                        raise AssertionError('Server failed to start:\n' + '\n'.join(self.lines))
                    continue
                if line.startswith('LISTENING '):
                    self.port = int(line.rsplit(':', 1)[1])
                    return
            raise AssertionError('Startup timed out:\n' + '\n'.join(self.lines))
        except BaseException:
            self.process.kill()
            self.process.wait()
            self.reader.join(timeout=2)
            raise

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=3)

    def rows(self):
        with self.log.open(newline='') as handle:
            return list(csv.DictReader(handle))

    def wait_rows(self, count):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            rows = self.rows()
            if len(rows) >= count:
                return rows
            if self.process.poll() is not None:
                raise AssertionError('Unexpected server exit: ' + '\n'.join(self.lines))
            time.sleep(0.02)
        raise AssertionError(f'Expected {count} logged rows; got {len(self.rows())}')

    def wait_status(self, predicate):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                line = self.output.get(timeout=0.1)
            except queue.Empty:
                continue
            if line.startswith('STATUS '):
                fields = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
                if predicate(fields):
                    return fields
        raise AssertionError('Expected status not reached:\n' + '\n'.join(self.lines))

    def stop(self):
        if self.process.poll() is None and os.name != 'nt':
            self.process.send_signal(signal.SIGTERM)
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('Shutdown timed out')
        finally:
            self.reader.join(timeout=2)
        if self.process.returncode != 0:
            raise AssertionError('Server exited unsuccessfully:\n' + '\n'.join(self.lines))
        final = next((line for line in reversed(self.lines) if line.startswith('FINAL ')), None)
        if final is None:
            raise AssertionError('Missing graceful shutdown summary:\n' + '\n'.join(self.lines))
        return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', final)}


class TcpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = None

    def tearDown(self):
        try:
            if self.server and self.server.process.poll() is None:
                self.server.stop()
        finally:
            self.temp.cleanup()

    def start(self, *args):
        self.server = Server(self.temp.name, *args)
        return self.server

    def test_split_coalesced_and_reconnect(self):
        server = self.start()
        with server.connect() as client:
            client.sendall(b'sensor,1,')
            time.sleep(0.03)
            client.sendall(b'12.5\nsensor,2,-4\r\nsensor,3,1e2\n')
        server.wait_rows(3)
        with server.connect() as client:
            client.sendall(b'sensor,4,0\n')
        rows = server.wait_rows(4)
        final = server.stop()
        self.assertEqual([int(r['sequence']) for r in rows], [1, 2, 3, 4])
        self.assertEqual([float(r['value']) for r in rows], [12.5, -4, 100, 0])
        self.assertEqual(final['processed'], 4)
        self.assertEqual(final['dropped'], 0)

    def test_malformed_and_truncated(self):
        server = self.start()
        with server.connect() as client:
            client.sendall(b'bad\nsensor,1,nan\nsensor,-1,4\nsensor,2,3x\nsensor,9,7\nsensor,10,')
        server.wait_rows(1)
        server.wait_status(lambda x: x['connected'] == 0 and x['truncated'] == 1)
        final = server.stop()
        self.assertEqual(final['malformed'], 4)
        self.assertEqual(final['truncated'], 1)
        self.assertEqual(final['processed'], 1)

    def test_oversized_client_does_not_stop_server(self):
        server = self.start()
        with server.connect() as client:
            client.sendall(b'x' * 257)
        server.wait_status(lambda x: x['oversized'] == 1)
        with server.connect() as client:
            client.sendall(b'healthy,1,3\n')
        server.wait_rows(1)
        final = server.stop()
        self.assertEqual(final['oversized'], 1)
        self.assertEqual(final['processed'], 1)

    def test_simultaneous_clients(self):
        server = self.start('--queue-capacity', '2048')
        def send(device):
            with server.connect() as client:
                client.sendall(''.join(f'{device},{n},{n / 10}\n' for n in range(100)).encode())
        # Futures propagate client failures into the test rather than losing them in threads.
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(send, ['a', 'b', 'c', 'd']))
        rows = server.wait_rows(400)
        final = server.stop()
        self.assertEqual({(r['device'], int(r['sequence'])) for r in rows},
                         {(d, n) for d in 'abcd' for n in range(100)})
        self.assertEqual(final['processed'], 400)
        self.assertEqual(final['dropped'], 0)

    def test_overload_accounting_and_shutdown_drain(self):
        server = self.start('--queue-capacity', '2', '--processing-delay-ms', '20')
        with server.connect() as client:
            client.sendall(''.join(f'fast,{n},1\n' for n in range(200)).encode())
        status = server.wait_status(lambda x: x['connected'] == 0 and x['dropped'] > 0)
        final = server.stop()
        self.assertEqual(final['processed'] + final['dropped'], 200)
        self.assertEqual(final['processed'], len(server.rows()))
        self.assertLessEqual(status['queue_high_water'], 2)

    def test_shutdown_with_open_partial_client(self):
        server = self.start()
        with server.connect() as client:
            client.sendall(b'pending,1,')
            server.wait_status(lambda x: x['connected'] == 1)
            final = server.stop()
        self.assertEqual(final['truncated'], 1)
        self.assertEqual(final['connected'], 0)

    def test_invalid_options_and_existing_log(self):
        for args in [['--port', '65536'], ['--port', '1junk'], ['--queue-capacity', '0'],
                     ['--unknown', 'x'], ['--log']]:
            result = subprocess.run([BINARY, *args], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, args)
        existing = Path(self.temp.name) / 'keep.csv'
        existing.write_text('keep me\n')
        result = subprocess.run([BINARY, '--port', '0', '--log', str(existing)],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(existing.read_text(), 'keep me\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
