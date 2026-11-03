import socket
import subprocess
import sys
import threading
import time
import unittest


PROBE = sys.argv.pop(1)


class HealthcheckTests(unittest.TestCase):
    def check_response(self, response, expected, delay=0, fragmented=False):
        errors = []
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", 8080))
            listener.listen(1)
            listener.settimeout(4)

            def serve():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(4)
                        request = b""
                        while b"\r\n\r\n" not in request:
                            chunk = connection.recv(4096)
                            if not chunk:
                                raise AssertionError("Probe closed before sending request")
                            request += chunk
                        self.assertEqual(
                            request,
                            b"GET /api/dashboard HTTP/1.1\r\n"
                            b"Host: 127.0.0.1:8080\r\nConnection: close\r\n\r\n",
                        )
                        time.sleep(delay)
                        if fragmented:
                            for byte in response:
                                connection.sendall(bytes([byte]))
                                time.sleep(0.002)
                        elif response:
                            connection.sendall(response)
                except Exception as error:
                    errors.append(error)

            server = threading.Thread(target=serve)
            server.start()
            try:
                start = time.monotonic()
                result = subprocess.run(
                    [PROBE], capture_output=True, text=True, timeout=4
                )
                elapsed = time.monotonic() - start
            finally:
                server.join(timeout=5)
            self.assertFalse(server.is_alive())
            if errors:
                raise errors[0]
            self.assertEqual(result.returncode, expected, result.stderr)
            if expected:
                self.assertIn("Health check failed:", result.stderr)
            if delay:
                self.assertIn("timed out", result.stderr)
                self.assertGreater(elapsed, 1.5)
                self.assertLess(elapsed, 3)

    def test_success_and_fragmented_status(self):
        self.check_response(b"HTTP/1.1 200 OK\r\n", 0, fragmented=True)
        self.check_response(b"HTTP/1.0 200 OK\r\n", 0)

    def test_non_success_status(self):
        self.check_response(b"HTTP/1.1 503 Unavailable\r\n", 1)

    def test_malformed_status(self):
        for response in (
            b"HTTP/1.1 2000 Invalid\r\n",
            b"HTTP/1.1 200 OK\n",
            b"x" * 128,
            b"",
        ):
            with self.subTest(response=response):
                self.check_response(response, 1)

    def test_timeout(self):
        self.check_response(b"", 1, delay=2.2)

    def test_connection_refused(self):
        # Reserve the port without listening so no other test can take it.
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 8080))
            result = subprocess.run(
                [PROBE], capture_output=True, text=True, timeout=4
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("Health check failed:", result.stderr)


if __name__ == "__main__":
    unittest.main()
