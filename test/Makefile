CC      := gcc
CFLAGS  := -Wall -Wextra -std=gnu17 -g -DFIRERED
# test/mocks shadows GBA-specific headers; include/ provides the real headers
INCLUDE := -I mocks -I ../include
SRCDIR  := ../src
TESTDIR := .

TESTS   := test_smoke test_packets test_starter test_dispatch test_send_throttle

.PHONY: all clean

all: $(TESTS)
	@for t in $(TESTS); do \
		echo "Running $$t ..."; \
		./$$t || exit 1; \
	done
	@echo "All tests passed."

STUBS := $(TESTDIR)/stubs.c

test_smoke: $(TESTDIR)/test_smoke.c $(SRCDIR)/multiplayer.c $(STUBS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^

test_packets: $(TESTDIR)/test_packets.c $(SRCDIR)/multiplayer.c $(STUBS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^

test_starter: $(TESTDIR)/test_starter.c $(SRCDIR)/multiplayer.c $(STUBS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^

test_dispatch: $(TESTDIR)/test_dispatch.c $(SRCDIR)/multiplayer.c $(STUBS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^

test_send_throttle: $(TESTDIR)/test_send_throttle.c $(SRCDIR)/multiplayer.c $(STUBS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^

clean:
	rm -f $(TESTS)
