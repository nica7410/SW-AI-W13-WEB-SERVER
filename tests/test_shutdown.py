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
        observer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        observer.bind(str(Path(tmp, "observer")))
        observer.listen()
        observer.settimeout(2)
        program.write_text("""#!/usr/bin/env python3
import os, socket, sys, time
leader = os.getpid()
if os.fork():
    os._exit(0)
witness = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
witness.connect(%r)
print(f"Content-Type: text/plain\\r\\nX-CGI-Pid: {leader}\\r\\nContent-Length: 24\\r\\n\\r", flush=True)
for i in range(24):
    try:
        sys.stdout.write("x")
        sys.stdout.flush()
    except BrokenPipeError:
        # Keep the witness open even if the server merely closes the CGI output.
        pass
    time.sleep(0.5)
""" % str(Path(tmp, "observer")))
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
            witness, _ = observer.accept()
            start = time.monotonic()
            server.terminate()
            with witness:
                assert server.wait(timeout=6.5) == 0
                witness.settimeout(0.5)
                assert witness.recv(1) == b"", "CGI descendant still holds its socket"
            elapsed = time.monotonic() - start
            assert elapsed >= 4.5, f"response interrupted before grace period: {elapsed:.2f}s"
            try:
                os.kill(child, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError("CGI child still exists after server exit")
            print(f"CGI reaped and descendant stopped, server exited in {elapsed:.2f}s")
        finally:
            observer.close()
            client.close()
            if server.poll() is None:
                server.kill()
                server.wait()


if __name__ == "__main__":
    test_shutdown()
