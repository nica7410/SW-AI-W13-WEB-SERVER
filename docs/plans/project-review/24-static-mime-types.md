# 24. 정적 자산 다섯 종류에 맞는 Content-Type 제공하기

선택 작업 · 이미지·웹폰트·PDF를 제공하는 정적 사이트 시연 · 외부 제안 5번 · 기준 커밋 `fb656d0` · 구현 완료

**반영 결과**

정적 MIME 표에 다섯 확장자를 추가했다. `test_mime`에서 새 매핑과 대문자 확장자 처리 및 미등록 형식의 기본값을 확인했다.

현재 표에는 과제에서 예시한 HTML·CSS·JS·PNG·JPG가 있다. SVG와 웹폰트 등은 기본값 `application/octet-stream`으로 나간다. 정적 서버에 자주 올리는 다섯 형식의 확장자와 미디어 타입을 연결한다. [현재 MIME 표](../../../src/mime.c) · [IANA 미디어 타입 등록부](https://www.iana.org/assignments/media-types/media-types.xhtml)

## 추가할 미디어 타입

| 확장자 | Content-Type |
|---|---|
| `.svg` | `image/svg+xml` |
| `.ico` | `image/vnd.microsoft.icon` |
| `.webp` | `image/webp` |
| `.woff2` | `font/woff2` |
| `.pdf` | `application/pdf` |

## 작업 순서

1. `mime_type()`의 기존 표에 다섯 행을 추가한다. 확장자 비교는 기존처럼 대소문자를 구분하지 않고, 알 수 없는 확장자의 기본값은 `application/octet-stream`으로 둔다.
2. 문자 인코딩은 파일 작성자가 보장하는 범위에서 선언한다. 서버는 `-r`로 임의의 디렉터리를 제공할 수 있고 텍스트 파일의 인코딩을 확인하지 않으므로, 이번 변경은 확장자 매핑으로 한정한다. 저장소의 기본 HTML은 이미 UTF-8을 선언한다. [기본 페이지](../../../www/index.html)

## 완료 기준과 최소 검증

기존 `test_mime`에 다섯 매핑을 추가하고 그중 하나는 대문자 확장자로 검사한다. 이 실행 파일에서 새 매핑과 기존의 미등록 확장자 기본값을 확인한다. 정적 응답이 `mime_type()` 반환값을 그대로 사용하므로 이 변경은 MIME 단위 검사 한 번으로 검증할 수 있다. [MIME 단위 검사](../../../tests/test_mime.c) · [정적 응답 호출부](../../../src/static_file.c)
