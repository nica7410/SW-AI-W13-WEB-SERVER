"""Check SIGTERM during a finite CGI response. Run from the repository root."""
import http.client
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def test_shutdown():
    with tempfile.TemporaryDirectory() as tmp:
        program = Path(tmp, "stream")
        program.write_text("""#!/usr/bin/env python3
import os, sys, time
print(f"Content-Type: text/plain\\r\\nX-CGI-Pid: {os.getpid()}\\r\\nContent-Length: 24\\r\\n\\r", flush=True)
for i in range(24):
    sys.stdout.write("x")
    sys.stdout.flush()
    time.sleep(0.5)
""")
        program.chmod(0o700)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server = subprocess.Popen([os.environ.get("SERVER_BIN", "./bin/server"), "-p", str(port),
                                   "-r", tmp, "-g", tmp, "-t", "1"])
        client = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        try:
            start = time.monotonic()
            while True:
                try:
                    client.connect()
                    break
                except ConnectionRefusedError:
                    if server.poll() is not None or time.monotonic() - start > 2:
                        raise
                    time.sleep(0.02)
            client.request("GET", "/cgi-bin/stream")
            response = client.getresponse()
            assert response.status == 200 and response.read(1) == b"x"
            child = int(response.getheader("X-CGI-Pid"))
            start = time.monotonic()
            server.terminate()
            assert server.wait(timeout=6.5) == 0
            elapsed = time.monotonic() - start
            assert elapsed >= 4.5, f"response interrupted before grace period: {elapsed:.2f}s"
            try:
                os.kill(child, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError("CGI child still exists after server exit")
            print(f"CGI reaped, server exited in {elapsed:.2f}s")
        finally:
            client.close()
            if server.poll() is None:
                server.kill()
                server.wait()


if __name__ == "__main__":
    test_shutdown()
