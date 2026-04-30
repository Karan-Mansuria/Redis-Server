# =============================================================================
# Makefile — Mini Redis Server
# EGC 301P Operating Systems Lab Mini Project
# =============================================================================
#
# Usage:
#   make          → compile everything into ./mini-redis
#   make run      → compile + run the server
#   make clean    → remove compiled binary and runtime files
#   make test     → compile and print how to test manually
#

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -g

# All source files to compile
SRCS    = src/server.c              \
          src/hashtable.c           \
          src/auth.c                \
          src/persistence.c         \
          src/expiry.c              \
          src/commands/dispatcher.c \
          src/commands/keyval.c     \
          src/commands/strings.c    \
          src/commands/ttl.c

TARGET  = mini-redis

# -----------------------------------------------------------------------------
# Default target: compile everything
# -----------------------------------------------------------------------------
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)
	@echo ""
	@echo "  ✓ Build successful → ./$(TARGET)"
	@echo "  Run with: make run"
	@echo "  Test with: telnet localhost 6379"
	@echo ""

# -----------------------------------------------------------------------------
# Run target: compile + start the server
# -----------------------------------------------------------------------------
run: all
	./$(TARGET)

# -----------------------------------------------------------------------------
# Clean target: remove build artefacts and runtime files
# NOTE: does NOT remove dump.rdb by default (would lose your data).
#       Use 'make cleanall' to also wipe runtime files.
# -----------------------------------------------------------------------------
clean:
	rm -f $(TARGET)

cleanall: clean
	rm -f dump.rdb server.log

# -----------------------------------------------------------------------------
# Test helper: print testing instructions
# -----------------------------------------------------------------------------
test: all
	@echo ""
	@echo "=== Mini Redis Manual Test Instructions ==="
	@echo ""
	@echo "Terminal 1 — Start server:"
	@echo "  ./mini-redis"
	@echo ""
	@echo "Terminal 2 — Admin client:"
	@echo "  telnet localhost 6379"
	@echo "  AUTH admin admin123"
	@echo "  SET counter 0"
	@echo "  INCR counter"
	@echo "  GET counter"
	@echo "  EXPIRE counter 15"
	@echo "  TTL counter"
	@echo "  SAVE"
	@echo "  BGSAVE"
	@echo ""
	@echo "Terminal 3 — Writer client:"
	@echo "  telnet localhost 6379"
	@echo "  AUTH writer writer123"
	@echo "  SET name Alice"
	@echo "  APPEND name Smith"
	@echo "  GET name"
	@echo ""
	@echo "Terminal 4 — Reader client:"
	@echo "  telnet localhost 6379"
	@echo "  AUTH reader reader123"
	@echo "  GET name"
	@echo "  EXISTS counter"
	@echo "  SET foo bar   # Should fail: Permission denied"
	@echo ""

.PHONY: all run clean cleanall test
