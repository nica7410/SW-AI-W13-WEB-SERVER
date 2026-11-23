#!/usr/bin/env bash
# 실행 중인 서버에 동시 접속 수를 바꿔 가며 부하를 주고 처리량·실패·최장 지연을 한 줄씩 출력한다.
#   URL=http://127.0.0.1:8080/index.html CONCURRENCY="1 8 32 64" REQUESTS=5000 KEEPALIVE=1 bash tests/load.sh
set -euo pipefail
url=${URL:-http://127.0.0.1:8080/index.html}
levels=${CONCURRENCY:-1 8 32 64}
requests=${REQUESTS:-5000}
keepalive=${KEEPALIVE:-0}

if command -v ab >/dev/null 2>&1; then
    flags=(-q -s 10 -n "$requests")
    [[ $keepalive == 1 ]] && flags+=(-k)
    echo "ab ${flags[*]} $url"
    for c in $levels; do
        if ! report=$(LC_ALL=C ab "${flags[@]}" -c "$c" "$url" 2>&1); then
            printf 'ab failed (c=%s):\n%s\n' "$c" "$report" >&2
            exit 1
        fi
        LC_ALL=C awk -v c="$c" '
            BEGIN {non_2xx=0}
            /^Requests per second:/ {rps=$4}
            /^Failed requests:/ {fail=$3}
            /^Non-2xx responses:/ {non_2xx=$3}
            /longest request/ {max=$2}
            END {
                if (rps !~ /^[0-9]+([.][0-9]+)?$/ || fail !~ /^[0-9]+$/ ||
                    max !~ /^[0-9]+$/ || non_2xx !~ /^[0-9]+$/) {
                    printf "invalid ab report (c=%s)\n", c > "/dev/stderr"
                    exit 1
                }
                printf "c=%-4s %10s req/s  failed=%s  non_2xx=%s  longest=%sms\n", c, rps, fail, non_2xx, max
            }' <<<"$report"
    done
elif command -v wrk >/dev/null 2>&1; then
    for c in $levels; do
        echo "== wrk -c$c"
        wrk -t1 -c"$c" -d"${DURATION:-3s}" "$url"
    done
else
    echo "Install Apache Bench (ab) or wrk to measure throughput and latency." >&2
    exit 1
fi
