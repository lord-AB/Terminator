# ─────────────────────────────────────────────────────────────────────
# Terminator — Makefile
# ─────────────────────────────────────────────────────────────────────
# Build system for the Terminator terminal emulator.
# Supports macOS (XQuartz/Homebrew) and Linux natively.
# ─────────────────────────────────────────────────────────────────────

CC       = gcc
TARGET   = terminator
BUILDDIR = build

# ── Compiler flags ──────────────────────────────────────────────────
CFLAGS   = -Wall -Wextra -g -pthread
CFLAGS  += -Iinclude

# ── Platform detection for X11 paths ────────────────────────────────
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS: try pkg-config first (Homebrew), fallback to XQuartz
    X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null || echo "-I/opt/X11/include")
    X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null || echo "-L/opt/X11/lib -lX11")
else
    # Linux: standard paths
    X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
    X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null || echo "-lX11")
endif

CFLAGS  += $(X11_CFLAGS)
LDFLAGS  = -lpthread $(X11_LIBS)

# On Linux, -lrt is needed for shm_open (not used currently, but kept for future)
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lrt
endif

# ── Source files (recursive wildcard) ───────────────────────────────
SRCS = $(shell find src -name '*.c')
OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRCS))

# ── Default target ──────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "✓ Built $(TARGET) successfully"
	@echo "  Run:  ./terminator                   (thread mode)"
	@echo "  Run:  ./terminator useProcess=True    (process mode)"
	@echo ""

# ── Pattern rule: compile .c → .o in build/ ────────────────────────
$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Tests ───────────────────────────────────────────────────────────
TEST_BIN = $(BUILDDIR)/test_gui_buffer
TEST_PARSER_BIN = $(BUILDDIR)/test_parser
TEST_REDIRECT_BIN = $(BUILDDIR)/test_redirect

test: $(TARGET) $(TEST_BIN) $(TEST_PARSER_BIN) $(TEST_REDIRECT_BIN)
	@echo "Running GUI unit tests..."
	./$(TEST_BIN)
	@echo "Running Multiline & Parser unit tests..."
	./$(TEST_PARSER_BIN)
	@echo "Running AST & Redirection unit tests..."
	./$(TEST_REDIRECT_BIN)
	@echo "Running verification echo test (thread mode)..."
	./$(TARGET) --test
	@echo "Running verification echo test (process mode)..."
	./$(TARGET) useProcess=True --test
	@echo "✓ All tests passed!"

$(TEST_BIN): tests/test_gui_buffer.c $(BUILDDIR)/gui/screen_buf.o $(BUILDDIR)/gui/tab_bar.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_PARSER_BIN): tests/test_parser.c $(BUILDDIR)/parser/multiline.o $(BUILDDIR)/parser/expand.o $(BUILDDIR)/parser/subst.o $(BUILDDIR)/parser/lexer.o $(BUILDDIR)/exec/shell_ctx.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_REDIRECT_BIN): tests/test_redirect.c $(BUILDDIR)/parser/lexer.o $(BUILDDIR)/parser/ast.o $(BUILDDIR)/exec/redirect.o $(BUILDDIR)/exec/pipeline.o $(BUILDDIR)/exec/shell_ctx.o $(BUILDDIR)/ipc/protocol.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Clean ───────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR) $(TARGET)

# ── Phony targets ──────────────────────────────────────────────────
.PHONY: all clean test

# ── Debug: print variables ──────────────────────────────────────────
print-%:
	@echo "$* = $($*)"
