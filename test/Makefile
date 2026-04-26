CC      := gcc
CFLAGS  := -Wall -Wextra -std=gnu17 -g -DFIRERED
# test/mocks shadows GBA-specific headers; include/ provides the real headers
INCLUDE := -I mocks -I ../include
SRCDIR  := ../src
TESTDIR := .

TESTS   := test_smoke

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

clean:
	rm -f $(TESTS)
