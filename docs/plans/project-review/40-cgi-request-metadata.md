# 40. CGI에 요청 버전·스크립트 이름·접속 정보 전달

기준 `cd65525` · 우선순위 보통 · 리뷰 ⑤ · 구현 완료

## 수정 범위

현재 CGI가 받는 메서드·쿼리·본문 정보에 `SERVER_PROTOCOL`, `SCRIPT_NAME`, `SERVER_NAME`, `SERVER_PORT`, `REMOTE_ADDR` 다섯 값을 추가한다. 스크립트가 자신을 호출한 공개 경로와 접속 정보를 알 수 있게 한다. 기존 환경 변수의 값, 명시적으로 허용한 변수 목록, 표준 입력으로 전달하는 본문은 기존 계약을 따른다. [CGI 실행](../../../src/cgi.c) · [CGI 메타 변수](https://www.rfc-editor.org/rfc/rfc3875.html#section-4.1)

## 구현과 완료 기준

1. 워커가 `fork()`하기 전에 변수 문자열을 준비한다. `SERVER_PROTOCOL`은 현재 요청 버전, `SCRIPT_NAME`은 쿼리를 뺀 논리적 CGI 경로다. 경로는 기존 URL 디코딩 규칙을 재사용해 한 번 해석한 값을 쓰고, 심볼릭 링크의 대상이나 macOS 임시 실행 경로가 노출되지 않게 한다. [파일 경로 해석](../../../src/local_path.c)
2. `REMOTE_ADDR`와 `SERVER_PORT`는 실제 연결 소켓의 상대 숫자 주소와 로컬 포트에서 얻는다. `SERVER_NAME`은 현재 요청의 검증된 Host에서 포트를 분리해 쓰고, Host가 없는 HTTP/1.0 요청은 로컬 숫자 주소를 쓴다. 전달 헤더에 대한 신뢰 정책이나 역방향 DNS 조회를 추가할 필요가 없다. IP 주소가 없는 내부 테스트 소켓은 얻을 수 있는 변수만 제공한다. [호스트 파싱](../../../src/proxy_internal.h)
3. `test_server_edges`의 기존 TCP 소켓 쌍과 임시 CGI 스크립트를 재사용한다. 쿼리가 붙은 요청 하나로 다섯 값을 응답에 출력해, 공개 경로·요청 버전·호스트·양 끝 소켓에서 확인한 값과 대조한다. 기존 CGI 본문과 실행 파일 검사는 같은 바이너리에 있다. [서버 경계 테스트](../../../tests/test_server_edges.c) · [TCP 소켓 쌍](../../../tests/tcp_pair.h)

## 반영 결과

`test_server_edges`가 TCP 연결로 임시 CGI를 실행해 다섯 환경 변수의 값을 대조했다. 공개 경로 `/cgi-bin/run%2Dlink?q=1`의 `SCRIPT_NAME`은 `/cgi-bin/run-link`였고, `SERVER_PORT`는 Host에 적힌 80과 별도로 소켓에서 확인한 수신 포트와 일치했다.
