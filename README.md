# SW-AI-W13-WEB-SERVER

POSIX 소켓 API만으로 처음부터 만든 C 웹서버 + Forward / Reverse Proxy (WEEK13 개인 프로젝트)

정적 파일과 CGI를 직접 제공하며, 설정에 따라 경로 기반 Reverse Proxy와 절대 URI 기반 Forward Proxy로도 동작합니다. 외부 HTTP·이벤트 루프 라이브러리 없이 표준 C 라이브러리와 POSIX API(`socket`, `pthread`, `poll`, `fork`/`execve`)만 사용합니다.

## 설계

| 요구사항 | 구현과 설계 선택 | 검증 근거 |
|---|---|---|
| TCP 수락과 동시 처리 | [수락 루프](src/server.c)가 연결을 [고정 크기 워커 풀](src/threadpool.c)에 넘깁니다. 새 연결이 기다리면 유휴 keep-alive 연결이 워커를 양보하고, 대기 큐까지 차면 새 연결에 503을 보냅니다. | [유휴 연결 양보·큐 초과 503](tests/functional.sh) |
| HTTP 파싱과 응답 | [파서](src/http_request.c)는 헤더의 끝을 읽은 뒤 Content-Length만큼 본문을 받습니다. [응답 작성기](src/http_response.c)는 상태와 본문 길이를 보냅니다. | [경계·시간 초과·연결 재사용](tests/test_http_request.c) |
| 정적 파일과 CGI | [정적 핸들러](src/static_file.c)는 루트 안의 파일을 제공하고, [CGI 핸들러](src/cgi.c)는 별도 프로세스의 출력을 중계합니다. | [루트 경계·CGI 응답](tests/test_server_edges.c) |
| Forward·Reverse Proxy | [Forward](src/forward_proxy.c)는 클라이언트를 대신해 원 서버에 접속합니다. [Reverse](src/proxy.c)는 서비스 앞에서 경로에 맞는 백엔드를 고르고 연결 실패 시 다음 후보를 시도합니다. Forward 캐싱은 과제의 선택 항목입니다. | [프록시 중계·장애 전환](tests/test_proxy_relay.c) |
| 종료와 자원 회수 | [서버](src/server.c)가 새 연결 수락을 멈춥니다. 유예가 끝나 CGI를 중단하면 같은 그룹에 종료 신호를 보내고 직접 실행한 자식을 회수합니다. | [SIGTERM과 하위 프로세스 정리](tests/test_shutdown.py) |

요청 크기와 시간 상한, 지원 메서드는 아래에 정리했습니다. [처리 범위와 한계](#처리-범위와-한계)

## 요청 처리 흐름

```
main ─ 인자 파싱, 시그널 설정 (SIGPIPE 무시, SIGINT/SIGTERM → 안전 종료)
 └ run_server ─ open_listenfd → threadpool_create → accept 루프(TCP_NODELAY) → threadpool_submit
      └ (워커) handle_connection ─ read_request (keep-alive 동안 반복, 대기 연결이 있으면 양보)
            ├ 절대 URI (http://...)   → forward_proxy_request  (-f 없으면 403)
            ├ 라우팅 규칙과 일치       → reverse_proxy_request  (→ 백엔드, Round-Robin)
            ├ /cgi-bin/...            → serve_cgi
            └ 그 외                   → serve_static
```

## 폴더 구조

```
src/
  main.c                    진입점: 인자 파싱, 시그널
  server.{h,c}              accept 루프, 연결 하나 처리(분기), keep-alive와 워커 양보
  server_internal.h         정적·CGI 루트 디렉터리 고정, 오류 응답 내부 API
  threadpool.{h,c}          스레드 풀, 대기 연결 확인
  net.{h,c}                 open_listenfd / open_clientfd, 마감 시간이 있는 DNS 조회·연결
  rio.{h,c}                 short count 처리 read/write
  deadline.{h,c}            단조 시계 기반 마감 시각, 마감이 있는 읽기·쓰기
  http_request.{h,c}        요청 파서 (http_request_internal.h: 테스트용 내부 API)
  http_fields.h             헤더 이름 문자·Connection 옵션 검사
  http_response.{h,c}       응답 생성, 오류 응답
  mime.{h,c}                확장자 → Content-Type
  static_file.{h,c}         정적 파일 서빙
  local_path.{h,c}          URL 디코딩, 루트 밖 경로·심볼릭 링크 차단
  cgi.{h,c}                 CGI 실행
  router.{h,c}              라우팅 규칙, Round-Robin
  proxy.{h,c}               프록시 공통 중계 + Reverse Proxy, 502/504
  forward_proxy.{h,c}       절대 URI 파싱 + Forward Proxy (proxy_internal.h: host:port 파서)
  response_stream.{h,c}     CGI·원 서버 응답 헤더 검증과 본문(chunked 포함) 중계 (upstream.h)
tests/                      단위 테스트(test_*.c), 기능(functional.sh), 부하(load.sh)
www/                        정적 파일 루트
cgi-bin/                    CGI 프로그램
conf/routes.conf            Reverse Proxy 라우팅 규칙
docs/plans/project-review/  리뷰 지적 사항별 수정 계획과 결과
.github/workflows/ci.yml    Ubuntu 단위·기능·종료·sanitizer 검사, macOS 일반 빌드·단위 검사
```

## 빌드 · 실행

WSL2(Ubuntu) / Linux / macOS

```bash
make            # bin/server, cgi-bin/adder
make test       # 단위 테스트
make asan       # build/asan, bin/asan에 ASan·UBSan 빌드 후 단위 테스트
make clean

./bin/server -p 8080 -r ./www                          # 웹서버
./bin/server -p 8080 -r ./www -c conf/routes.conf      # Reverse Proxy
./bin/server -p 8080 -r ./www -f                       # Forward Proxy 허용
```

Reverse Proxy는 `/api/` 접두사를 포함한 경로를 그대로 전달합니다. 기본 설정으로 두 백엔드를 시연하려면 각각의 정적 루트에 `api/index.html`을 만들고 서로 다른 내용을 적은 뒤 8001번과 8002번에 띄웁니다.

```bash
curl http://127.0.0.1:8080/api/
curl --noproxy '' -x http://127.0.0.1:8080 http://127.0.0.1:8001/api/
curl -d 'a=1&b=2' http://127.0.0.1:8080/cgi-bin/adder    # 3
```

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `-p` | 8080 | 포트 |
| `-r` | ./www | 지정한 루트를 열 수 없으면 시작 실패. 옵션을 생략했을 때 기본 루트가 없으면 경고 후 기동 |
| `-g` | ./cgi-bin | CGI 폴더를 열 수 없으면 경고 후 기동 |
| `-c` | 없음 | 라우팅 설정 파일 (지정하면 Reverse Proxy 동작) |
| `-f` | 꺼짐 | Forward Proxy 허용 |
| `-v` | 꺼짐 | 요청마다 접근 로그 한 줄을 stderr에 출력 |
| `-t` | 8 | 워커 스레드 수 |
| `-q` | 1024 | 연결 대기 큐 크기 (가득 차면 503). 열린 파일 한도(`ulimit -n`)보다 크면 한도를 올리거나 큐를 줄이고 경고를 출력 |

### 접근 로그로 분산과 장애 전환 확인하기

`./bin/server -c conf/routes.conf -v`로 실행하면 다음과 같은 로그가 출력됩니다. `first`는 처음 연결을 시도한 백엔드, `connected`는 연결에 성공한 백엔드입니다. 세 번째 줄에서는 8001번 연결에 실패해 8002번에서 응답을 받았습니다.

```text
access event=request method=GET path=/api/ status=200 outcome=complete first=127.0.0.1:8001 connected=127.0.0.1:8001 attempts=1
access event=request method=GET path=/api/ status=200 outcome=complete first=127.0.0.1:8002 connected=127.0.0.1:8002 attempts=1
access event=request method=GET path=/api/ status=200 outcome=complete first=127.0.0.1:8001 connected=127.0.0.1:8002 attempts=2
access event=overload method=- path=- status=503 outcome=complete first=- connected=- attempts=0
```

큐가 가득 차면 요청을 읽기 전에 503을 보내므로 메서드와 경로가 `-`로 표시됩니다. 중간에 전송이 끊기면 `outcome=aborted`가 남습니다. 그때 `status=-`이면 최종 헤더를 다 보내기 전이고, `status=200`이면 200 헤더를 보낸 뒤입니다. 경로에서 쿼리를 빼고 본문과 요청 헤더도 생략합니다.

## 검증

Ubuntu에서는 `fb656d0`의 빌드, 단위 검사, HTTP·프록시 기능 검사, 종료 검사, sanitizer 검사가 통과했습니다. macOS에서는 `a606e86`의 일반 빌드와 단위 검사가 통과했습니다. [Ubuntu CI 실행 기록](https://github.com/nica7410/SW-AI-W13-WEB-SERVER/actions/runs/45123268839) · [macOS CI 실행 기록](https://github.com/nica7410/SW-AI-W13-WEB-SERVER/actions/runs/45125742774)

| 명령 | 확인하는 동작 |
|---|---|
| `make test` | HTTP 요청 라인·헤더·크기 상한·헤더/본문 마감(408), MIME 매핑, 절대 URI, 최장 접두사·Round-Robin, 프록시 중계·마감, 파일 경계·CGI·TCP 오류 응답, 스레드 풀 대기 판단 |
| `bash tests/functional.sh` | 실제 서버 5개에서 정적 파일·MIME, CGI GET/POST, keep-alive·HEAD, 동시 접속, Forward 허용 여부, Reverse 분산·장애 전환·502/504, 워커 1개 서버의 유휴 연결 양보, 큐가 넘칠 때 503 응답 전체 전달 확인 |
| `make asan` | AddressSanitizer와 UBSan을 적용해 빌드하고 단위 테스트 실행 |
| `python3 tests/test_shutdown.py` | SIGTERM 유예, CGI 직접 자식 회수, 같은 그룹에서 응답을 계속 쓰는 하위 프로세스 종료 확인 |
| `bash tests/load.sh` | 실행 중인 서버에 동시 접속 수를 바꿔 가며 요청을 보내 처리량·실패·최장 지연 측정 |

기능 테스트는 `make` 후 실행하며 Bash, curl, Python 3과 연속된 빈 포트 6개가 필요합니다. 기본 포트는 18080~18085이고 `PORT=19080 bash tests/functional.sh`처럼 바꿀 수 있습니다.

sanitizer 서버와 CGI로 기능 테스트를 실행하려면 `SERVER_BIN=./bin/asan/server CGI_DIR=./bin/asan/cgi-bin bash tests/functional.sh`를 사용합니다. 일반 실행 파일은 `bin/server`와 `cgi-bin/adder`에 있습니다.

부하 스크립트는 `ab`(없으면 `wrk`)를 사용합니다. `URL`, `CONCURRENCY`(공백으로 구분한 목록), `REQUESTS`, `KEEPALIVE=1`로 조건을 바꿉니다.

`ab` 요약의 `failed`는 연결·읽기·응답 길이 등의 실패 수이고, `non_2xx`는 HTTP 상태가 2xx 범위 밖인 응답 수입니다. 503 응답의 길이까지 달라지면 같은 요청이 두 지표에 모두 포함됩니다. 도구 실행이 실패하거나 필수 지표가 누락되면 스크립트는 오류로 종료합니다.

```bash
URL=http://127.0.0.1:8080/index.html CONCURRENCY="1 8 32 64 100 256" bash tests/load.sh
URL=http://127.0.0.1:8080/index.html CONCURRENCY="1 8 9 32 64 100 256" KEEPALIVE=1 bash tests/load.sh
```

### 부하 측정 결과 (13~18번 반영 후, 커밋 전 작업 트리)

외부 리뷰어가 `ad86c5f` 위에 13~18번을 반영한 작업 트리를 Docker `gcc:14` 컨테이너에 복사해 측정했습니다. 측정 시점은 연결 거부(`ECONNREFUSED`)를 5초 제외 대상에서 빼기 전입니다.

서버는 기본 설정(`-t 8 -q 1024`)으로 정적 파일 `/index.html`을 제공했습니다. ApacheBench(`ab -q -s 10 -n 5000`)로 동시 연결 수(`-c`)와 연결 재사용 여부(`-k`)를 바꿔 각 조건을 한 번씩 측정했습니다. 표의 단위는 초당 요청 수입니다.

| 동시 연결 수 | 연결 재사용 없음 | keep-alive (`-k`) |
|---:|---:|---:|
| 1 | 5,538 | 12,481 |
| 8 | 12,977 | 39,501 |
| 9 | — | 10,564 |
| 32 | 12,980 | 13,010 |
| 100 | 12,774 | 12,889 |

측정한 9개 조건 모두 `failed=0`, `non_2xx=0`이었습니다. 동시 연결 9개에서는 keep-alive 결과만 보고됐습니다.

<a id="측정-결과-최종-코드"></a>

### 부하 측정 결과 (`3a6b120`)

아래 부하 수치와 메모리 검사 기록은 개선 전 커밋 `3a6b120` 기준입니다. 이후 변경의 검증 결과는 [08번부터 12번까지의 작업 기록](docs/plans/project-review/)에 있습니다.

환경: Windows 10의 Docker Desktop(WSL2) 위 `gcc:14` 컨테이너, 서버 기본 설정(`-t 8 -q 1024`), 클라이언트와 서버가 같은 컨테이너, 대상 `/index.html`(319바이트), 요청 5,000건, 조건마다 1회 측정. 같은 머신에서 클라이언트도 CPU를 나눠 쓰므로 수치의 절대값보다 경향을 보는 용도입니다.

| 동시 연결 수 | 연결 재사용 없음 | 최장 지연 | keep-alive(`-k`) | 최장 지연 |
|---:|---:|---:|---:|---:|
| 1 | 5,490건/초 | 1ms | 12,991건/초 | 1ms |
| 8 | 12,202건/초 | 2ms | 38,253건/초 | 1ms |
| 9 | — | — | 10,165건/초 | 2ms |
| 32 | 11,966건/초 | 6ms | 12,068건/초 | 10ms |
| 64 | 11,944건/초 | 7ms | 11,887건/초 | 8ms |
| 100 | 12,206건/초 | 10ms | 12,019건/초 | 12ms |
| 256 | 11,212건/초 | 41ms | 11,730건/초 | 29ms |

모든 조건에서 실패 0건입니다. 워커 수(8)보다 동시 keep-alive 연결이 많으면 서버가 연결을 양보하면서 닫기 때문에 처리량이 연결 재사용 없음 수준으로 내려갑니다. 대신 특정 연결이 유휴 타임아웃(5초)만큼 멈추는 일은 없습니다.

느린 클라이언트와 종료 처리도 같은 환경에서 확인했습니다.

| 시나리오 | 결과 |
|---|---|
| 연결 8개가 요청 헤더를 1초에 1바이트씩 보냄 → 9번째 정상 요청 | 4.8초 후 200. 느린 연결은 5초에 408 |
| 연결 8개가 POST 본문을 4초에 1바이트씩 보냄 → 9번째 정상 요청 | 4.8초 후 200. 느린 연결은 5초에 408 |
| 유휴 keep-alive 연결이 있는 상태에서 SIGTERM | 30ms 후 종료 코드 0 |
| 워커 2개 서버의 큐에 요청 없는 연결 40개가 쌓인 상태에서 SIGTERM | 26ms 후 종료 코드 0 (조용한 연결은 처리하지 않고 닫음) |
| `-q 16`으로 띄운 서버에 `ab -c 200`, `ab -c 1000` (요청 5,000건) | 모두 완료. 넘친 연결은 503 응답을 받음 (각각 2,112건, 497건) |
| `ulimit -n 256`에서 기본 설정으로 실행 후 `ab -c 300` | 큐를 96으로 줄였다는 경고 출력. 요청 3,000건 모두 완료(503 574건), accept 실패 0건 |

### 후속 외부 부하 측정 (`ad86c5f`)

외부 리뷰어가 Docker `gcc:14` 컨테이너의 저장소 복사본에서 보고한 keep-alive(`-k`) 결과입니다.

| 동시 연결 수 | 처리량 | 비-2xx 응답 수 |
|---:|---:|---:|
| 8 | 39,413건/초 | 0 |
| 100 | 12,962건/초 | 0 |

두 조건에서 관측한 처리량은 이전 표보다 높았습니다. 외부 요약에는 정확한 명령줄, 요청 수, 대상 파일, 워커·큐 설정, 반복 횟수, 최장 지연과 `failed` 값이 기재되지 않았습니다. `non_2xx`는 HTTP 상태를 기준으로 집계합니다.

### 메모리 검사

- `make -j4 asan`: 단위 테스트 7종이 AddressSanitizer + UBSan 빌드에서 오류 없이 통과했습니다.
- sanitizer 빌드의 `functional.sh`도 통과했습니다.
- sanitizer 빌드 서버 두 개(Reverse/Forward 프록시 + 백엔드)에 keep-alive 부하(`ab -k -c 32`), 동시 100 연결 부하, Reverse·CGI POST·Forward·404 요청을 섞어 보낸 뒤 SIGTERM으로 종료했습니다. 두 서버 모두 종료 코드 0, 누수·메모리 오류·정의되지 않은 동작 보고 0건이었습니다.

## 처리 범위와 한계

- HTTP/1.0·1.1에서 GET·POST·HEAD를 처리하며 그 외 메서드에는 501을 반환합니다. URI는 2047바이트까지 허용하고 초과하면 414를 보냅니다. 헤더는 고정 배열에 최대 32개, 한 줄 8KB, 값 버퍼 1KB로 제한하며 상한을 넘기면 431입니다. 본문 상한은 1MB(413)입니다.
- 핸들러는 성공 시 0, 오류 응답이 필요하면 HTTP 상태 코드, 전송 중 연결을 닫아야 하면 -1을 반환합니다. 워커가 반환값을 보고 오류 응답을 보냅니다.
- 정적 파일은 GET·HEAD로 제공하고 POST에는 405를 보냅니다. CGI와 프록시는 POST 본문을 바이트 단위로 전달합니다.
- 정적 응답은 연결을 재사용할 수 있습니다. CGI와 프록시 응답은 `Connection: close`로 끝납니다.
- **과부하:** 연결 대기 큐(기본 1024)가 가득 차면 accept 스레드가 503을 보내고 송신만 닫은 뒤, 클라이언트가 닫을 때까지(최대 1초) 입력을 버리다가 close합니다. 이렇게 기다리는 연결이 64개면 accept를 잠시 멈추고 새 연결을 커널 listen backlog에 둡니다. 시작할 때 `워커 × 8 + 96 + 큐 크기`만큼의 열린 파일 한도를 확보하고, hard 한도 때문에 안 되면 큐를 줄입니다.
- **워커 양보:** 응답을 보낼 때 쉬는 워커가 없고 큐에 기다리는 연결이 있으면 그 응답을 `Connection: close`로 보내고 연결을 넘깁니다. 다음 요청을 기다리는 유휴 연결도 100ms 이상 조용하고 기다리는 연결이 있으면 닫습니다. 두 경우 모두 송신을 먼저 닫고 잠깐 입력을 버려, 클라이언트가 이어 보낸 요청 때문에 RST로 응답을 잃지 않게 합니다. 유휴 연결을 서버가 먼저 닫는 것은 HTTP가 허용하는 동작이지만, 클라이언트가 막 다음 요청을 보낸 순간과 겹치면 그 요청은 응답 없이 끊길 수 있습니다(클라이언트가 재시도해야 함).
- **시간 제한:** 새 연결의 첫 요청과 keep-alive의 다음 요청은 첫 바이트를 최대 5초 기다립니다. 첫 바이트가 오면 요청 라인부터 헤더 끝까지 5초, 본문 전체에 다시 5초를 적용하고 넘기면 408을 보냅니다. 종료 신호를 받으면 이미 도착한 요청만 처리하고 조용한 연결은 바로 닫습니다. 그래서 느린 클라이언트 하나가 워커를 붙잡는 시간은 최대 약 15초이고, 워커 수만큼 느린 연결이 몰리면 그동안 다른 요청이 기다립니다. 이는 스레드 풀 구조의 한계이며 이벤트 루프(epoll) 방식이면 없어집니다.
- 본문 마감 5초는 1MB 본문 기준 약 200KB/s보다 느린 업로드를 408로 끊습니다.
- **응답 중계와 종료** · CGI는 실행 시작부터, 두 프록시는 요청 전달 후 응답을 읽기 시작할 때부터 헤더·본문·클라이언트 쓰기에 총 30초를 적용합니다. I/O가 멈추면 5초 후 중단합니다. 진행 중인 큰 중계도 30초를 초과하면 연결이 닫힙니다. SIGINT·SIGTERM으로 수락을 멈추면 진행 중인 작업은 공통 유예 5초 안에 마칠 수 있습니다. 유예가 끝나면 워커가 남은 중계를 중단하고 연결을 닫습니다. 정적 파일은 평상시 전송 시간에 전체 상한이 없습니다.
- 두 프록시는 DNS 조회부터 최종 응답 헤더까지 5초를 공유합니다. Reverse Proxy는 연결할 때마다 남은 시간을 남은 후보 수로 나누고, 연결에 실패하면 다음 후보를 시도합니다. 모두 거부되면 502, 연결 시간 초과 후 모든 후보가 실패하거나 전체 5초가 지나면 504입니다. 연결 성공 후에는 전체 마감까지 요청을 보내고 응답 헤더를 받습니다. 전송을 시작한 요청은 재시도하지 않습니다.
- Forward Proxy는 `http://` 절대 URI를 처리합니다. HTTPS CONNECT와 chunked 요청 본문은 501입니다. HTTP/1.1 원 서버의 chunked 응답은 인코딩된 상태로 중계합니다. 캐싱은 구현하지 않았습니다.
- Reverse Proxy에 백엔드가 둘 이상이면 연결 시간 초과나 목적지 도달 불가 등의 오류가 난 후보를 5초간 제외합니다. 연결 거부(`ECONNREFUSED`)는 제외 기간을 시작하거나 연장하지 않습니다. 따라서 모두 연결을 거부해 502를 보냈더라도 다음 요청에서 재기동한 백엔드에 바로 연결할 수 있습니다. 남은 후보끼리 라운드로빈을 적용하고 제외 기간이 끝나면 요청 하나가 해당 서버의 복구를 확인합니다. 모든 후보가 제외 또는 복구 확인 중이면 502를 즉시 반환합니다. 실패를 동시에 발견한 요청은 각각 지연될 수 있습니다.
- 정적 파일과 CGI에서 루트 안을 가리키는 심볼릭 링크를 사용할 수 있습니다. Linux·WSL은 검증한 파일 디스크립터로 CGI를 실행합니다. macOS의 CGI 실행은 경로를 다시 참조하므로 같은 보강이 남아 있습니다.
- CGI에는 메서드, 쿼리, 본문 길이와 타입을 환경 변수로 전달합니다. POST 본문은 임시 파일을 통해 표준 입력으로 전달하므로 큰 본문과 프로그램 출력이 서로를 막지 않습니다.
- CGI 요청마다 전용 프로세스 그룹을 만듭니다. 중계 오류, 응답 마감 또는 서버 종료 유예 만료로 CGI를 중단하면 같은 그룹에 `SIGKILL`을 보내고, 서버가 직접 실행한 자식은 `waitpid()`로 회수합니다. CGI가 정상적으로 끝난 뒤 백그라운드에 남긴 하위 프로세스는 서버 종료 후에도 실행될 수 있습니다.
- 끝에 `/`가 없는 디렉터리 요청은 리다이렉트 없이 403입니다. 요청 줄바꿈은 CRLF만 받습니다.
