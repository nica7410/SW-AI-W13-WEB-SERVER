# [Makefile] make: 서버 + CGI 빌드 / make test: 단위 테스트 / make asan: 메모리 검사 빌드 / make clean
CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -g -O2
BUILD_CFLAGS  = $(CFLAGS) -pthread -Isrc -MMD -MP
BUILD_LDFLAGS = $(LDFLAGS) -pthread

ifeq ($(SANITIZE),1)
BUILD_CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD_LDFLAGS += -fsanitize=address,undefined
endif

SRC   := $(filter-out src/main.c,$(wildcard src/*.c))
OBJ   := $(SRC:src/%.c=build/%.o)
TESTS := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

all: bin/server cgi-bin/adder

bin/server: build/main.o $(OBJ) | bin
	$(CC) $(BUILD_CFLAGS) -o $@ $^ $(BUILD_LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(BUILD_CFLAGS) -c $< -o $@

cgi-bin/adder: cgi-bin/adder.c
	$(CC) $(BUILD_CFLAGS) -o $@ $< $(BUILD_LDFLAGS)

build/test_%: tests/test_%.c $(OBJ) | build
	$(CC) $(BUILD_CFLAGS) -o $@ $< $(OBJ) $(BUILD_LDFLAGS)

# Only the parser test observes each call before forwarding it to the real RIO reader.
build/http_request_observed.o: src/http_request.c | build
	$(CC) $(BUILD_CFLAGS) -Drio_readline_until=test_request_readline_until -c $< -o $@

build/test_http_request: tests/test_http_request.c build/http_request_observed.o $(filter-out build/http_request.o,$(OBJ)) | build
	$(CC) $(BUILD_CFLAGS) -o $@ $< $(filter %.o,$^) $(BUILD_LDFLAGS)

build/test_server_edges: | cgi-bin/adder

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# AddressSanitizer + UBSan: 메모리 누수·버퍼 오버플로우·정의되지 않은 동작 검사
asan:
	$(MAKE) clean
	$(MAKE) SANITIZE=1 all test

bin build:
	mkdir -p $@

clean:
	rm -rf bin build cgi-bin/adder

-include $(wildcard build/*.d)

.PHONY: all test asan clean
