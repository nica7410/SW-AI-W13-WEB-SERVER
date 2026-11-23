#!/usr/bin/env bash
# make 이후 저장소 루트에서 bash tests/functional.sh 로 실행한다.
set -eu
cd "$(dirname "$0")/.."
port=${PORT:-18080}
a=$((port + 1))
b=$((port + 2))
slow=$((port + 3))
fair=$((port + 4))
full=$((port + 5))
base="http://127.0.0.1:$port"
tmp=$(mktemp -d)
pids=()

cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf -- "$tmp"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

fetch() { curl --noproxy '*' --max-time 8 -sS "$@"; }

start() {
    local number=$1
    shift
    "${SERVER_BIN:-./bin/server}" -p "$number" -g "${CGI_DIR:-./cgi-bin}" "$@" >"$tmp/$number.log" 2>&1 &
    local pid=$!
    pids+=("$pid")
    for ((i=0; i<60; i++)); do
        if ! kill -0 "$pid" 2>/dev/null; then cat "$tmp/$number.log" >&2; return 1; fi
        if fetch -f "http://127.0.0.1:$number/" >/dev/null 2>&1; then return 0; fi
        sleep 0.05
    done
    echo "server on $number did not start" >&2
    return 1
}

expect() {
    if [[ $1 != "$2" ]]; then
        echo "expected '$2', received '$1' ($3)" >&2
        exit 1
    fi
}

for dir in a b; do
    mkdir -p "$tmp/$dir/api"
    echo "$dir" >"$tmp/$dir/index.html"
    echo "$dir" >"$tmp/$dir/api/index.html"
done
printf '/api/ 127.0.0.1:%s 127.0.0.1:%s\n/slow/ 127.0.0.1:%s\n' "$a" "$b" "$slow" >"$tmp/routes"
start "$a" -r "$tmp/a"
start "$b" -r "$tmp/b"
start "$port" -r ./www -c "$tmp/routes" -f -v
start "$fair" -r ./www -t 1
start "$full" -r ./www -t 1 -q 1 -v

for pair in 'index.html text/html' 'style.css text/css' 'app.js application/javascript'; do
    read -r file type <<<"$pair"
    expect "$(fetch -o "$tmp/body" -w '%{http_code} %{content_type}' "$base/$file")" "200 $type" "$file"
    cmp "$tmp/body" "www/$file"
done
expect "$(fetch -o /dev/null -w '%{http_code}' "$base/missing")" 404 missing
expect "$(fetch --path-as-is -o /dev/null -w '%{http_code}' "$base/../Makefile")" 403 traversal
expect "$(fetch -f -d 'a=1&b=2' "$base/cgi-bin/adder")" 3 CGI
expect "$(fetch -f "$base/cgi-bin/adder?b=7&a=-2")" 5 CGI
expect "$(fetch -f "$base/api/?trace=private-marker")" a round-robin
expect "$(fetch -f "$base/api/")" b round-robin

proxy_get() {
    curl --noproxy '' --max-time 8 -sS -x "http://127.0.0.1:$1" "${@:2}"
}
expect "$(proxy_get "$port" -f "http://127.0.0.1:$b/")" b forward
expect "$(proxy_get "$port" -f -d 'a=12&b=30' "http://127.0.0.1:$b/cgi-bin/adder")" 42 forward-post
expect "$(proxy_get "$a" -o /dev/null -w '%{http_code}' "http://127.0.0.1:$b/")" 403 forward-disabled

jobs_to_wait=()
for file in index.html style.css app.js; do
    fetch -f "$base/$file" >"$tmp/$file" &
    jobs_to_wait+=("$!")
done
for pid in "${jobs_to_wait[@]}"; do wait "$pid"; done
for file in index.html style.css app.js; do cmp "$tmp/$file" "www/$file"; done

python3 - "$port" "$slow" "$fair" "$full" <<'PY'
import http.client
from contextlib import closing
import socket
import sys

port, slow, fair, full = map(int, sys.argv[1:])
import time
def exchange(data):
    with socket.create_connection(("127.0.0.1", port), timeout=8) as client:
        client.sendall(data)
        client.shutdown(socket.SHUT_WR)
        with client.makefile("rb") as reply:
            return reply.read()

for request, status in [
    (b"BAD\r\n\r\n", 400),
    (b"GET / HTTP/1.1\r\n\r\n", 400),
    (b"POST / HTTP/1.1\r\nHost: h\r\n\r\n", 411),
    (b"POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
]:
    assert exchange(request).split(b" ", 2)[1] == str(status).encode()

# 한 TCP 쓰기에 담은 두 요청의 경계, HEAD 본문 생략, 두 번째 요청 후 close.
reply = exchange(b"HEAD /missing HTTP/1.1\r\nHost: h\r\n\r\n"
                 b"GET /style.css HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n")
first, rest = reply.split(b"\r\n\r\n", 1)
assert first.startswith(b"HTTP/1.1 404 ") and rest.startswith(b"HTTP/1.1 200 ")
with open("www/style.css", "rb") as css:
    assert rest.split(b"\r\n\r\n", 1)[1] == css.read()

# listen만 하는 백엔드는 접속에는 성공하지만 응답 헤더를 주지 않는다.
with socket.socket() as silent:
    silent.bind(("127.0.0.1", slow))
    silent.listen()
    idle = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    idle.request("HEAD", "/index.html")
    response = idle.getresponse()
    assert response.status == 200 and int(response.getheader("Content-Length")) > 0
    assert response.read() == b""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    conn.request("GET", "/slow/")
    assert conn.getresponse().status == 504
    conn.close()
    idle.sock.settimeout(2)
    assert idle.sock.recv(1) == b""
    idle.close()

# 최종 헤더를 보낸 후 원 서버 본문이 일찍 끝나면 200과 중단을 함께 기록한다.
with socket.socket() as origin, closing(http.client.HTTPConnection("127.0.0.1", port, timeout=3)) as proxy:
    origin.bind(("127.0.0.1", 0))
    origin.listen()
    origin.settimeout(3)
    proxy.request("GET", f"http://127.0.0.1:{origin.getsockname()[1]}/short")
    source, _ = origin.accept()
    with source, source.makefile("rb") as inbound:
        while inbound.readline() not in (b"\r\n", b""):
            pass
        source.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc")
    response = proxy.getresponse()
    assert response.status == 200 and response.read(5) == b"abc"

# 워커가 1개뿐인 서버: 유휴 keep-alive 연결이 워커를 붙잡고 있어도 새 연결은 유휴 타임아웃(5초)을 기다리지 않는다.
held = http.client.HTTPConnection("127.0.0.1", fair, timeout=8)
held.request("GET", "/index.html")
first = held.getresponse()
assert first.status == 200 and first.getheader("Connection") == "keep-alive"
first.read()
started = time.monotonic()
other = http.client.HTTPConnection("127.0.0.1", fair, timeout=8)
other.request("GET", "/style.css", headers={"Connection": "close"})
assert other.getresponse().status == 200
assert time.monotonic() - started < 2, "queued connection waited for the idle timeout"
other.close()
held.sock.settimeout(2)
assert held.sock.recv(1) == b""   # 양보하면서 닫은 유휴 연결
held.close()

# 워커 1개·큐 1개 서버에 동시 연결 20개: 넘친 연결도 RST 없이 503 응답 전체를 받는다.
clients = [socket.create_connection(("127.0.0.1", full), timeout=8) for _ in range(20)]
for c in clients:
    c.sendall(b"GET /index.html HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n")
statuses = []
for c in clients:
    with c, c.makefile("rb") as reply:
        data = reply.read()
    head, _, body = data.partition(b"\r\n\r\n")
    length = int(head.split(b"Content-Length: ")[1].split(b"\r\n")[0])
    assert len(body) == length, data
    statuses.append(int(head.split(b" ")[1]))
assert set(statuses) <= {200, 503} and 503 in statuses and 200 in statuses, statuses
PY

kill "${pids[0]}"
wait "${pids[0]}"
unset 'pids[0]'
expect "$(fetch -f "$base/api/")" b failover
kill "${pids[1]}"
wait "${pids[1]}"
unset 'pids[1]'
expect "$(fetch -o /dev/null -w '%{http_code}' "$base/api/")" 502 unavailable
start "$b" -r "$tmp/b"
expect "$(fetch -f "$base/api/")" b recovery-after-refusal
access_line() {
    grep -Fxq "access $2" "$tmp/$1.log" || { echo "missing access log: $2" >&2; exit 1; }
}
rr='event=request method=GET path=/api/ status=200 outcome=complete'
for peer in "$a" "$b"; do
    access_line "$port" "$rr first=127.0.0.1:$peer connected=127.0.0.1:$peer attempts=1"
done
access_line "$port" "$rr first=127.0.0.1:$a connected=127.0.0.1:$b attempts=2"
access_line "$port" 'event=request method=GET path=/short status=200 outcome=aborted first=- connected=- attempts=0'
access_line "$full" 'event=overload method=- path=- status=503 outcome=complete first=- connected=- attempts=0'
if grep -q 'private-marker' "$tmp/$port.log" || grep -q '^access ' "$tmp/$a.log"; then
    echo "query or disabled access log was written" >&2
    exit 1
fi
echo "functional tests passed"
