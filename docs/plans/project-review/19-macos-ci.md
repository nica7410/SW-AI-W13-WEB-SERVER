# 19. macOS에서 일반 빌드와 단위 검사 확인하기

우선 작업 1 · 외부 제안 2번 · 기준 커밋 `fb656d0` · 설정 반영 · macOS 러너 검증 대기

**반영 결과**

Ubuntu와 같은 경고 옵션으로 `make`와 `make test`를 실행하는 독립 macOS 작업을 추가했다. 워크플로 diff에서 작업 순서와 공통 옵션을 확인했다. macOS 러너에서의 성공 여부는 이 변경을 포함하는 다음 push 또는 PR 실행에서 확인한다.

README는 macOS를 실행 환경으로 안내한다. DNS 조회 대기에는 Apple 전용 분기가 있고 CGI 실행 방식도 Linux와 다르다. 현재 Ubuntu CI는 통과했으므로 남은 질문은 같은 소스가 macOS에서 컴파일되고 기존 단위 검사를 통과하는지다. [실행 환경](../../../README.md#빌드--실행) · [DNS 대기](../../../src/net.c) · [CGI 실행](../../../src/cgi.c) · [Ubuntu 실행 결과](https://github.com/nica7410/SW-AI-W13-WEB-SERVER/actions/runs/45123268839)

## 작업 범위와 순서

1. 기존 워크플로에 독립된 `macos-latest` 작업을 추가한다. 체크아웃 후 일반 빌드와 같은 경고 옵션으로 `make`, 이어서 `make test`를 실행한다. 플랫폼 공통 단위 검사가 이미 Makefile에 모여 있으므로 이 두 명령을 그대로 사용한다. [CI 설정](../../../.github/workflows/ci.yml) · [Makefile](../../../Makefile) · [GitHub 러너 목록](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
2. macOS에서 실패하면 첫 컴파일 진단이나 실패한 단위 사례를 기준으로 원인을 좁힌다. 예를 들어 CGI 실행에서 차이가 나면 해당 OS 분기와 `test_server_edges`의 기대 동작을 함께 대조한다. Linux에서만 보장하는 동작은 README의 플랫폼 설명과 맞춘다.
3. 워크플로 반영과 실제 러너 통과를 구분해 기록한다. 다음 통상적인 push 또는 PR에서 생성된 macOS 작업을 확인한 뒤 README의 검증 안내에 확인된 커밋과 실행 링크를 넣는다.

## 완료 기준과 최소 검증

워크플로 변경은 YAML diff에서 러너, 작업 순서와 경고 옵션을 확인한다. 실제 검증의 완료 기준은 같은 macOS 작업에서 일반 빌드와 기존 단위 검사가 모두 성공한 기록이다. 원격 실행 전에는 결과를 대기 상태로 표시한다.
