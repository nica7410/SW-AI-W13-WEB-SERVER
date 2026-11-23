# 10. 일반 빌드와 sanitizer 빌드의 산출물 분리하기

외부 리뷰 5번 · 기준 커밋 `3a6b120` · 구현 순서 3/5

**구현 결과** · Docker에서 sanitizer 모드의 `test_server_edges`가 전용 CGI로 통과했다. 일반 실행 파일 해시 보존, sanitizer 실행 파일에만 ASan 심볼 존재, 소스 한 개 수정 후 일반 증분 링크 성공 및 sanitizer 파일 해시 보존을 확인했다.

외부 리뷰에서 `make asan` 직후 `make`를 실행해도 서버에 ASan 심볼이 남았다. 소스 하나를 변경한 뒤 일반 링크가 `__asan_report_store4`를 찾지 못해 실패했다. 기존 05번 계획은 병렬 빌드에서 정리 순서를 바로잡았다. 이번 변경은 두 모드가 같은 파일을 생성하는 문제를 다룬다. [Makefile](../../../Makefile) · [기존 05번 계획](05-parallel-asan-build.md)

## 확정 범위

| 산출물 | 일반 모드 | ASan·UBSan 모드 |
|---|---|---|
| 오브젝트, 관찰용 파서 오브젝트, 해당 의존성 파일 | `build/normal/` | `build/asan/` |
| 서버와 CGI adder | `bin/server`, `cgi-bin/adder` | `bin/asan/server`, `bin/asan/cgi-bin/adder` |
| 단위 테스트 실행 파일 | 기존 `build/test_*` | `build/asan/test_*` |

`make`, `make test`, `make asan`과 기존 일반 실행 경로를 유지한다. `make asan`은 해당 모드의 서버·CGI를 만들고 기존 단위 테스트를 실행한다. 두 모드 전환에 사용자의 `make clean`이 필요 없어야 한다.

## 구현 순서

1. 모드별 디렉터리 변수로 컴파일, 링크, 헤더 의존성 생성·포함, 테스트 목록과 실행 경로를 함께 구성한다. 현재 단일 명령으로 컴파일·링크하는 CGI도 모드별 오브젝트를 거쳐 링크한다. 기존 작업 트리에서 처음 일반 빌드를 하면 새 디렉터리에 오브젝트가 생성되고, 이를 선행 조건으로 둔 서버와 CGI가 다시 링크되어 이전 실행 파일을 교체한다.
2. `asan`에서 선행 전체 정리를 제거하고 같은 규칙에 sanitizer 모드를 지정한다. 명시적인 `make clean`은 두 모드와 이전 방식의 생성물을 정리한다. 호출자가 넘긴 플래그와 병렬 make의 선행 조건을 유지한다.
3. 실제 adder를 호출하는 `test_server_edges`에는 현재 모드의 CGI 디렉터리를 전달한다. 기능 스크립트는 서버 경로와 CGI 디렉터리를 지정할 수 있게 하고, 기본 실행은 기존 경로를 사용한다. README에는 sanitizer 서버로 기능을 확인할 때의 두 경로를 함께 적는다. [CGI 테스트](../../../tests/test_server_edges.c) · [기능 스크립트](../../../tests/functional.sh) · [실행 문서](../../../README.md#검증)

## 완료 기준과 최소 검증

임시 복사본에서 일반 서버와 CGI를 한 번 빌드하고 해시를 기록한다. 이어 sanitizer 모드에서는 서버·CGI와 `test_server_edges`만 빌드해 해당 테스트 한 번으로 CGI 경로를 확인한다. 일반 파일의 해시가 그대로인지, 두 서버 중 sanitizer 서버에만 ASan 참조가 있는지 검사한다. 같은 복사본에서 공통 소스 하나의 수정 시각을 바꿔 일반 `make`를 한 번 실행한다. 일반 서버의 증분 링크가 성공하고 sanitizer 서버 해시가 유지되어야 한다. 공개 `make asan`이 격리된 모드의 `all test`로 이어지는지는 레시피로 확인한다.
