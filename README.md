# SW-AI-W13-WEB-SERVER

POSIX 소켓 API만으로 처음부터 만든 C 웹서버 + Forward / Reverse Proxy (WEEK13 개인 프로젝트)

정적 파일과 CGI를 직접 제공하며, 설정에 따라 경로 기반 Reverse Proxy와 절대 URI 기반 Forward Proxy로도 동작합니다. 외부 HTTP·이벤트 루프 라이브러리 없이 표준 C 라이브러리와 POSIX API(`socket`, `pthread`, `poll`, `fork`/`execve`)만 사용합니다.

## 설계

| 항목 | 선택 | 이유 |
|---|---|---|
| 동시성 | 스레드 풀 (`pthread`, 생산자-소비자 큐) | 스레드 생성 비용 없음, 워커 수로 동시 처리량 제한, 공유 상태(Round-Robin 카운터) 사용 쉬움 |
| keep-alive 공정성 | 큐에 기다리는 연결이 있으면 유휴 연결을 양보 | 워커 하나가 유휴 연결 하나를 붙잡는 스레드 풀의 약점을 보완 (아래 "처리 범위" 참고) |
| 프록시 | **Reverse + Forward** | Reverse: 경로 prefix 라우팅 + Round-Robin + 502/504. Forward: 절대 URI 요청을 원 서버로 중계 (`-f`로 켤 때만) |
| 동적 처리 | CGI (`fork` + `execve`) | 서버와 분리된 프로그램 실행, CSAPP Tiny와 같은 방식 |
| 반환 규칙 | `0` 성공 · 양수 = 보낼 HTTP 오류 코드 · `-1` 연결 종료 | 오류 응답을 `handle_connection` 한 곳에서만 보냄 |
| 헤더 저장 | 고정 배열 32개 | malloc/free 없음, 상한 초과 시 바로 431 |
| 크기 상한 | URI 2047바이트까지 허용(2048바이트부터 414) · 헤더 줄 8KB · 헤더 32개(431) · 헤더 값 1KB(431) · 본문 1MB(413) | 저장 배열 2048바이트에 문자열 종료 문자가 포함됨 |
| 시간 제한 | 다음 요청 대기 5초 · 헤더 전체 5초 · 본문 전체 5초(넘으면 408) · 프록시 응답 헤더까지 5초 | 느린 클라이언트(slowloris)가 워커를 오래 붙잡지 못하게 함 |
| 과부하 | 큐가 가득 차면 503을 보낸 뒤 클라이언트가 닫을 때까지 기다렸다 close. 큐 크기는 열린 파일 한도에 맞춰 자동 조정 | 503 응답이 RST에 잘리지 않고, fd가 모자라 accept가 실패하는 일이 없음 |
| HTTP 범위 | GET · POST · HEAD (그 외 501), HTTP/1.0·1.1, keep-alive, chunked 요청 미지원 | |
| 설정 | CLI 인자 + `conf/routes.conf` | |

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
.github/workflows/ci.yml    push·PR마다 make(-Werror) → make test → make asan
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
| `-t` | 8 | 워커 스레드 수 |
| `-q` | 1024 | 연결 대기 큐 크기 (가득 차면 503). 열린 파일 한도(`ulimit -n`)보다 크면 한도를 올리거나 큐를 줄이고 경고를 출력 |

## 검증

| 명령 | 확인하는 동작 |
|---|---|
| `make test` | HTTP 요청 라인·헤더·크기 상한·헤더/본문 마감(408), MIME 매핑, 절대 URI, 최장 접두사·Round-Robin, 프록시 중계·마감, 파일 경계·CGI·TCP 오류 응답, 스레드 풀 대기 판단 |
| `bash tests/functional.sh` | 실제 서버 5개에서 정적 파일·MIME, CGI GET/POST, keep-alive·HEAD, 동시 접속, Forward 허용 여부, Reverse 분산·장애 전환·502/504, 워커 1개 서버의 유휴 연결 양보, 큐가 넘칠 때 503 응답 전체 전달 확인 |
| `make asan` | AddressSanitizer와 UBSan을 적용해 빌드하고 단위 테스트 실행 |
| `python3 tests/test_shutdown.py` | CGI 응답 진행 중 SIGTERM 유예와 직접 실행한 자식 회수 확인 |
| `bash tests/load.sh` | 실행 중인 서버에 동시 접속 수를 바꿔 가며 요청을 보내 처리량·실패·최장 지연 측정 |

기능 테스트는 `make` 후 실행하며 Bash, curl, Python 3과 연속된 빈 포트 6개가 필요합니다. 기본 포트는 18080~18085이고 `PORT=19080 bash tests/functional.sh`처럼 바꿀 수 있습니다.

sanitizer 서버와 CGI로 기능 테스트를 실행하려면 `SERVER_BIN=./bin/asan/server CGI_DIR=./bin/asan/cgi-bin bash tests/functional.sh`를 사용합니다. 일반 실행 파일은 `bin/server`와 `cgi-bin/adder`에 있습니다.

부하 스크립트는 `ab`(없으면 `wrk`)를 사용합니다. `URL`, `CONCURRENCY`(공백으로 구분한 목록), `REQUESTS`, `KEEPALIVE=1`로 조건을 바꿉니다.

`ab` 요약의 `failed`는 연결·읽기·응답 길이 등의 실패 수이고, `non_2xx`는 HTTP 상태가 2xx 범위 밖인 응답 수입니다. 503 응답의 길이까지 달라지면 같은 요청이 두 지표에 모두 포함됩니다. 도구 실행이 실패하거나 필수 지표가 누락되면 스크립트는 오류로 종료합니다.

```bash
URL=http://127.0.0.1:8080/index.html CONCURRENCY="1 8 32 64 100 256" bash tests/load.sh
URL=http://127.0.0.1:8080/index.html CONCURRENCY="1 8 9 32 64 100 256" KEEPALIVE=1 bash tests/load.sh
```

### 측정 결과 (최종 코드)

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

### 메모리 검사

- `make -j4 asan`: 단위 테스트 7종이 AddressSanitizer + UBSan 빌드에서 오류 없이 통과했습니다.
- sanitizer 빌드의 `functional.sh`도 통과했습니다.
- sanitizer 빌드 서버 두 개(Reverse/Forward 프록시 + 백엔드)에 keep-alive 부하(`ab -k -c 32`), 동시 100 연결 부하, Reverse·CGI POST·Forward·404 요청을 섞어 보낸 뒤 SIGTERM으로 종료했습니다. 두 서버 모두 종료 코드 0, 누수·메모리 오류·정의되지 않은 동작 보고 0건이었습니다.

## 처리 범위와 한계

- 정적 파일은 GET·HEAD로 제공하고 POST에는 405를 보냅니다. CGI와 프록시는 POST 본문을 바이트 단위로 전달합니다.
- 정적 응답은 연결을 재사용할 수 있습니다. CGI와 프록시 응답은 `Connection: close`로 끝납니다.
- **과부하:** 연결 대기 큐(기본 1024)가 가득 차면 accept 스레드가 503을 보내고 송신만 닫은 뒤, 클라이언트가 닫을 때까지(최대 1초) 입력을 버리다가 close합니다. 이렇게 기다리는 연결이 64개면 accept를 잠시 멈추고 새 연결을 커널 listen backlog에 둡니다. 시작할 때 `워커 × 8 + 96 + 큐 크기`만큼의 열린 파일 한도를 확보하고, hard 한도 때문에 안 되면 큐를 줄입니다.
- **워커 양보:** 응답을 보낼 때 쉬는 워커가 없고 큐에 기다리는 연결이 있으면 그 응답을 `Connection: close`로 보내고 연결을 넘깁니다. 다음 요청을 기다리는 유휴 연결도 100ms 이상 조용하고 기다리는 연결이 있으면 닫습니다. 두 경우 모두 송신을 먼저 닫고 잠깐 입력을 버려, 클라이언트가 이어 보낸 요청 때문에 RST로 응답을 잃지 않게 합니다. 유휴 연결을 서버가 먼저 닫는 것은 HTTP가 허용하는 동작이지만, 클라이언트가 막 다음 요청을 보낸 순간과 겹치면 그 요청은 응답 없이 끊길 수 있습니다(클라이언트가 재시도해야 함).
- **시간 제한:** 새 연결의 첫 요청과 keep-alive의 다음 요청은 첫 바이트를 최대 5초 기다립니다. 첫 바이트가 오면 요청 라인부터 헤더 끝까지 5초, 본문 전체에 다시 5초를 적용하고 넘기면 408을 보냅니다. 종료 신호를 받으면 이미 도착한 요청만 처리하고 조용한 연결은 바로 닫습니다. 그래서 느린 클라이언트 하나가 워커를 붙잡는 시간은 최대 약 15초이고, 워커 수만큼 느린 연결이 몰리면 그동안 다른 요청이 기다립니다. 이는 스레드 풀 구조의 한계이며 이벤트 루프(epoll) 방식이면 없어집니다.
- 본문 마감 5초는 1MB 본문 기준 약 200KB/s보다 느린 업로드를 408로 끊습니다.
- **응답 중계와 종료** · CGI는 실행 시작부터, 두 프록시는 요청 전달 후 응답을 읽기 시작할 때부터 헤더·본문·클라이언트 쓰기에 총 30초를 적용합니다. I/O가 멈추면 5초 후 중단합니다. 진행 중인 큰 중계도 30초를 초과하면 연결이 닫힙니다. SIGINT·SIGTERM으로 수락을 멈추면 진행 중인 작업은 공통 유예 5초 안에 마칠 수 있습니다. 유예가 끝나면 워커가 연결을 닫고 직접 실행한 CGI 자식을 종료·회수합니다. 정적 파일은 평상시 전송 시간에 전체 상한이 없습니다.
- 두 프록시는 DNS 조회부터 최종 응답 헤더까지 5초를 공유합니다. Reverse Proxy는 연결할 때마다 남은 시간을 남은 후보 수로 나누고, 연결에 실패하면 다음 후보를 시도합니다. 모두 거부되면 502, 연결 시간 초과 후 모든 후보가 실패하거나 전체 5초가 지나면 504입니다. 연결 성공 후에는 전체 마감까지 요청을 보내고 응답 헤더를 받습니다. 전송을 시작한 요청은 재시도하지 않습니다.
- Forward Proxy는 `http://` 절대 URI를 처리합니다. HTTPS CONNECT와 chunked 요청 본문은 501입니다. HTTP/1.1 원 서버의 chunked 응답은 인코딩된 상태로 중계합니다. 캐싱은 구현하지 않았습니다.
- 정적 파일과 CGI에서 루트 안을 가리키는 심볼릭 링크를 사용할 수 있습니다. Linux·WSL은 검증한 파일 디스크립터로 CGI를 실행합니다. macOS의 CGI 실행은 경로를 다시 참조하므로 같은 보강이 남아 있습니다.
- CGI에는 메서드, 쿼리, 본문 길이와 타입을 환경 변수로 전달합니다. POST 본문은 임시 파일을 통해 표준 입력으로 전달하므로 큰 본문과 프로그램 출력이 서로를 막지 않습니다.
- 끝에 `/`가 없는 디렉터리 요청은 리다이렉트 없이 403입니다. 요청 줄바꿈은 CRLF만 받습니다.
