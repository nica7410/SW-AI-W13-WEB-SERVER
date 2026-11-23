# cd65525 리뷰 후속 작업 범위

기준은 `dev`의 `cd65525`다. 33~43번을 작업 트리에 반영했다. 각 계획서 마지막에 해당 동작과 검증 결과를 기록했다.

## 항목별 계획서

- [33. HTTP/1.0 원 서버 응답의 전송 인코딩 검사](33-upstream-http10-transfer-encoding.md)
- [34. 204 응답을 보내기 전에 금지된 길이 헤더 검사](34-no-content-response-framing.md)
- [35. CGI가 Status를 두 번 보내면 500 반환](35-cgi-duplicate-status.md)
- [36. 잘못 쓴 HTTP 버전을 400으로 처리](36-request-version-syntax.md)
- [37. Content-Length 문법을 확인한 뒤 본문 상한 적용](37-content-length-error-precedence.md)
- [38. 접근 로그에서 긴 미지원 메서드 식별](38-unsupported-method-access-log.md)
- [39. 설정 파일에서 중복 라우팅 접두사 경고](39-duplicate-route-warning.md)
- [40. CGI에 요청 버전·스크립트 이름·접속 정보 전달](40-cgi-request-metadata.md)
- [41. DNS 조회 실패 시 주소 목록의 소유권 정리](41-dns-result-ownership.md)
- [42. README에 청크 제한 순서·모듈·검증 버전 명시](42-readme-implementation-limits.md)
- [43. 로컬 도구 디렉터리를 Git 제외 규칙에 추가](43-ignore-local-tool-directories.md)

## 적용할 설계 기준

- 동시성은 고정 pthread 워커 풀과 기존 수락 루프를 사용한다. 과부하 시 503 전체 응답 전달, keep-alive 워커 양보, 정적 파일의 유휴 마감과 종료 유예가 기존 동작의 기준이다. 이벤트 루프 전환이나 전송률 정책은 별도 성능 설계에 해당한다.
- Reverse Proxy는 쿼리를 제외한 경로 문자열의 최장 접두사와 라운드로빈을 사용한다. 동일 접두사는 경고만 추가한다. Forward Proxy의 활성화 방식, 요청 대상과 백엔드 경로, 재시도·쿨다운·Via 정책도 현재 설계를 따른다.
- HTTP/1.0·1.1 지원, 유효한 요청 본문을 읽은 뒤 분기하는 순서, 기존 중간 응답 처리와 응답 청크 중계가 호환성의 기준이다. 32번의 단독 공백 거절은 클라이언트 요청에 한정된다. 이번 응답 수정은 금지된 프레이밍 헤더와 중복 CGI 상태에 적용한다. [30번 기준](30-connection-content-length.md) · [32번 기준](32-request-chunk-size-whitespace.md)
- CGI는 기존 프로세스 그룹, 시그널 처리, 검증한 실행 파일과 표준 입력 전달 방식을 따른다. 40번은 기존 스크립트에 추가 정보를 제공한다. CGI 리다이렉트, HTTP 헤더 일괄 전달, 자식 회수·FD 정리 방식 교체는 별도로 설계할 호환성 변경이다.
- 패킷 수, 503 송신 지연, 대용량 정적 전송률에 대한 추정은 성능 개선의 근거로 사용하지 않는다. 메서드 이름 진단은 기존 접근 로그 필드를 활용하며 `connected`와 `duration_ms`의 의미는 기존 문서를 따른다. [측정 조건과 설계 설명](../../../README.md)

## 반영 후 검증

| 항목 | 검사 | 확인한 사항 |
|---|---|---|
| 33·34·35·41 | `test_proxy_relay` | 잘못된 헤더 거절 시 송신 0바이트, 정상 204, 기존 DNS 마감 사례. 완료와 마감이 겹칠 때의 소유권은 코드로 대조했다. |
| 36·37·38 | `test_http_request` | 요청 라인과 헤더 오류 코드, 거절된 메서드의 기록값 |
| 39 | `test_router` | 중복 경고와 첫 규칙 선택 |
| 40 | `test_server_edges` | CGI 프로세스가 실제로 받은 요청 환경 변수 |
| 42·43 | 문서 diff·링크 및 `git check-ignore` | 문서의 근거와 로컬 경로에 적용된 제외 규칙 |

Windows에서 Linux Docker(`gcc:14`)로 위 네 테스트를 각각 한 번 실행해 모두 통과했다. `git check-ignore -v`로 두 위치의 제외 규칙을 확인했다. 기준 커밋의 Ubuntu·macOS CI 기록은 [README 검증 절](../../../README.md#검증)에 있다.
