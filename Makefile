# [Makefile] make: 서버 + CGI 빌드 / make test: 단위 테스트 / make asan: 메모리 검사 빌드 / make clean
CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -g -O2
BUILD_CFLAGS  = $(CFLAGS) -pthread -Isrc -MMD -MP
BUILD_LDFLAGS = $(LDFLAGS) -pthread

ifeq ($(SANITIZE),1)
OBJDIR := build/asan
TESTDIR := build/asan
BINDIR := bin/asan
CGIDIR := bin/asan/cgi-bin
BUILD_CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD_LDFLAGS += -fsanitize=address,undefined
else
OBJDIR := build/normal
TESTDIR := build
BINDIR := bin
CGIDIR := cgi-bin
endif

SRC   := $(filter-out src/main.c,$(wildcard src/*.c))
OBJ   := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
TESTS := $(patsubst tests/%.c,$(TESTDIR)/%,$(wildcard tests/test_*.c))

all: $(BINDIR)/server $(CGIDIR)/adder

$(BINDIR)/server: $(OBJDIR)/main.o $(OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(BUILD_LDFLAGS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(BUILD_CFLAGS) -c $< -o $@

$(OBJDIR)/adder.o: cgi-bin/adder.c | $(OBJDIR)
	$(CC) $(BUILD_CFLAGS) -c $< -o $@

$(CGIDIR)/adder: $(OBJDIR)/adder.o | $(CGIDIR)
	$(CC) $(CFLAGS) -o $@ $< $(BUILD_LDFLAGS)

$(TESTDIR)/test_server_edges: TEST_FLAGS = -DTEST_CGI_DIR='"$(CGIDIR)"'
$(TESTDIR)/test_%: tests/test_%.c $(OBJ) | $(TESTDIR)
	$(CC) $(BUILD_CFLAGS) $(TEST_FLAGS) -o $@ $< $(OBJ) $(BUILD_LDFLAGS)

# Only the parser test observes each call before forwarding it to the real RIO reader.
$(OBJDIR)/http_request_observed.o: src/http_request.c | $(OBJDIR)
	$(CC) $(BUILD_CFLAGS) -Drio_readline_until=test_request_readline_until -c $< -o $@

$(TESTDIR)/test_http_request: tests/test_http_request.c $(OBJDIR)/http_request_observed.o $(filter-out $(OBJDIR)/http_request.o,$(OBJ)) | $(TESTDIR)
	$(CC) $(BUILD_CFLAGS) -o $@ $< $(filter %.o,$^) $(BUILD_LDFLAGS)

$(TESTDIR)/test_server_edges: | $(CGIDIR)/adder

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# AddressSanitizer + UBSan: 메모리 누수·버퍼 오버플로우·정의되지 않은 동작 검사
asan:
	$(MAKE) SANITIZE=1 all test

$(sort $(BINDIR) $(TESTDIR) $(OBJDIR) $(CGIDIR)):
	mkdir -p $@

clean:
	rm -rf bin build cgi-bin/adder cgi-bin/adder.d

-include $(wildcard $(OBJDIR)/*.d) $(wildcard $(TESTDIR)/test_*.d)

.PHONY: all test asan clean
