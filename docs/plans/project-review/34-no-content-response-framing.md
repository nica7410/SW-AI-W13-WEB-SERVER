# 34. 204 응답을 보내기 전에 금지된 길이 헤더 검사

기준 `cd65525` · 우선순위 높음 · 리뷰 ⑥ · 구현 완료

## 수정 범위

현재는 204의 본문을 생략하는 시점에 헤더가 이미 전송돼 있다. 최종 상태가 204이면 `Content-Length`와 `Transfer-Encoding`의 **존재 여부**를 검사한다. 길이 값이 0이어도 같은 규칙을 적용한다. [응답 작성 순서](../../../src/response_stream.c) · [Content-Length 규칙](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.6) · [Transfer-Encoding 규칙](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.1)

## 구현과 완료 기준

1. 모든 헤더를 읽고 최종 상태를 확정한 뒤, 첫 최종 응답 바이트를 쓰기 전에 검사한다. CGI의 `Status`가 길이 필드보다 뒤에 올 수도 있다. 잘못된 원 서버 응답은 502, CGI 출력은 기존 오류 경로를 통해 500으로 처리한다.
2. HEAD와 304에는 각각의 헤더 규칙을 적용한다. 정상 204는 헤더 끝에서 완료한다.
3. `test_proxy_relay`의 소켓 검사에서 원 서버 204와 전송 인코딩, CGI 204와 길이 0을 각각 다룬다. CGI에는 유효한 `Content-Type`을 넣어 오류 원인을 분리한다. 두 오류의 최종 송신 0바이트와 금지 필드가 없는 204 한 건의 정상 완료를 확인한다. [중계 테스트](../../../tests/test_proxy_relay.c)

## 반영 결과

최종 상태와 모든 헤더를 확인한 후 204의 길이 필드를 거절한다. `test_proxy_relay`에서 원 서버의 전송 인코딩은 502, CGI의 `Content-Length: 0`은 500이며 두 경우 모두 송신량이 0이었다. 프레이밍 필드가 없는 204는 헤더 끝에서 정상 완료됐다.
