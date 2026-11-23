"""Check server startup, completed CGI responses and SIGTERM from the repository root."""
import http.client
import os
from pathlib import Path
import resource
import socket
import subprocess
import tempfile
import time


def server_command(root, workers):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    command = [os.environ.get("SERVER_BIN", "./bin/server"), "-p", str(port),
               "-r", root, "-g", root, "-t", str(workers)]
    return command, port


def start_server(root, workers=1, **popen_options):
    command, port = server_command(root, workers)
    server = subprocess.Popen(command, **popen_options)
    client = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    end = time.monotonic() + 2
    try:
        while True:
            try:
                client.connect()
                return server, client
            except ConnectionRefusedError:
                if server.poll() is not None or time.monotonic() >= end:
                    raise
                time.sleep(0.02)
    except BaseException:
        client.close()
        server.kill()
        server.wait()
        raise


def test_startup_fd_budget():
    with tempfile.TemporaryDirectory() as tmp:
        body = b"fd budget\n"
        Path(tmp, "index.html").write_bytes(body)
        command, _ = server_command(tmp, 8)
        rejected = subprocess.run(command, stderr=subprocess.PIPE, text=True, timeout=3,
                                  preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (160, 160)))
        assert rejected.returncode == 1, rejected
        assert "limit 160 is too low for 8 workers (minimum 161" in rejected.stderr, rejected.stderr

        server, client = start_server(tmp, workers=8, stderr=subprocess.PIPE, text=True,
                                      preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (161, 161)))
        try:
            client.request("GET", "/index.html")
            response = client.getresponse()
            assert response.status == 200 and response.read() == body
        finally:
            client.close()
            server.terminate()
            _, warning = server.communicate(timeout=7)
        assert "queue size 1024 exceeds the open file limit 161; using 1" in warning, warning
        print("FD limit 160: startup rejected; 161: queue size 1, static GET 200")


def test_completed_cgi_close():
    with tempfile.TemporaryDirectory() as tmp:
        body = b"response\n" * 131072
        Path(tmp, "body").write_bytes(body)
        gate = Path(tmp, "ready")
        os.mkfifo(gate)
        script = Path(tmp, "stream")
        script.write_text(f"""#!/usr/bin/env python3
import sys
from pathlib import Path
print("Content-Type: text/plain\\r\\nContent-Length: {len(body)}\\r\\n\\r", flush=True)
with open({str(gate)!r}, "rb") as ready:
    ready.read(1)
sys.stdout.buffer.write(Path({str(Path(tmp, "body"))!r}).read_bytes())
""")
        script.chmod(0o700)
        server, client = start_server(tmp)
        try:
            # Holding both ends lets the test release the CGI without blocking on open().
            with open(gate, "r+b", buffering=0) as ready, client.sock.makefile("rb") as wire:
                client.request("GET", "/cgi-bin/stream")
                assert wire.readline() == b"HTTP/1.1 200 OK\r\n"
                headers = http.client.parse_headers(wire)
                assert headers["Connection"] == "close" and int(headers["Content-Length"]) == len(body)
                client.sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
                ready.write(b"x")
                assert wire.read() == body, "expected the complete final response followed by EOF"
        finally:
            client.close()
            server.terminate()
            server.wait(timeout=7)


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
        server, client = start_server(tmp)
        try:
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
    test_startup_fd_budget()
    test_completed_cgi_close()
    test_shutdown()
