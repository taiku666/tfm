CC = gcc
CSTD = -std=c11
WARN_FLAGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow

# CFLAGS is user/CI-overridable (e.g. `make CFLAGS='-O0 -g'` for a
# debug build, or `make CFLAGS='-fsanitize=address,undefined -g'`); the
# warning flags and language standard above are not part of it, so
# overriding CFLAGS can't accidentally drop them.
#
# Default -O2: release binaries (built via plain `make`, including the
# ones attached to GitHub releases) were previously unoptimized (-O0,
# the implicit default with no -O flag at all) with no one having
# deliberately decided that. -D_FORTIFY_SOURCE=2 is a no-op without at
# least -O1 (its checks need __builtin_object_size, which needs
# optimization info to resolve) - now that there's a default -O2, it
# does something. Together these surfaced 5 real warnings invisible at
# -O0: two ignored chown()/fchown() return values (now handled properly,
# were already deliberately best-effort) and three genuine
# -Wformat-truncation false positives from GCC losing precise bound
# info through a struct-pointer/pointer-offset access - see unsized()
# in tfm_common.h for how those are suppressed without hiding a real bug.
CFLAGS ?= -O2 -D_FORTIFY_SOURCE=2
ALL_CFLAGS = $(WARN_FLAGS) $(CSTD) $(CFLAGS)

IFLAGS = -Iinclude

SRC_DIR = src
GUI_SRC_DIR = src_gui
BUILD_DIR = build
GUI_BUILD_DIR = $(BUILD_DIR)/gui
BIN_DIR = bin

TARGET = $(BIN_DIR)/tfm
GUI_TARGET = $(BIN_DIR)/tfm-gui

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

CORE_SRCS = config.c dir.c editor.c fileops.c opener.c shell.c tfm_common.c
CORE_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

GUI_SRCS = $(wildcard $(GUI_SRC_DIR)/*.c)
GUI_OBJS = $(patsubst $(GUI_SRC_DIR)/%.c,$(GUI_BUILD_DIR)/%.o,$(GUI_SRCS))

# Automated unit tests (tests/test_*.c). Each one is its own self-contained
# test binary (its own main(), via tests/test.h) linked against the real
# $(CORE_OBJS) - so tests run against the actual compiled fileops.c/
# tfm_common.c/etc, not a reimplementation - rather than one combined
# binary, so a crash or exit() in one test file's process can't take out
# unrelated test files.
TEST_DIR = tests
TEST_BUILD_DIR = $(BUILD_DIR)/tests
TEST_SRCS = $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(TEST_BUILD_DIR)/%,$(TEST_SRCS))

# Auto-generated header-dependency files (see -MMD -MP below): included at
# the bottom so editing a header (e.g. include/panel.h) correctly triggers
# a rebuild of every .c file that includes it, not just the ones make's
# own $(SRC_DIR)/%.c pattern rule already covers.
DEPS = $(OBJS:.o=.d) $(GUI_OBJS:.o=.d)

GTK_PKGS = gtk4 libadwaita-1
GTK_CFLAGS := $(shell pkg-config --cflags $(GTK_PKGS) 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs $(GTK_PKGS) 2>/dev/null)

PREFIX ?= /usr/local
INSTALL_DIR = $(PREFIX)/bin
# DESTDIR is a separate staging-root prefix (packaging convention, e.g.
# `make install DESTDIR=/tmp/pkgroot PREFIX=/usr`) - left empty for a
# normal, non-packaged install.
DESTDIR ?=

OMARCHY_HOOK_DIR = $(HOME)/.config/omarchy/hooks/theme-set.d
OMARCHY_HOOK_SRC = contrib/omarchy-hooks/tfm-gui-reload-theme

.PHONY: scan all clean install uninstall tfm-gui check-gtk-deps install-gui uninstall-gui \
        install-gui-theme-hook uninstall-gui-theme-hook test unit-test smoke-test test-pty lint asan asan-test

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(ALL_CFLAGS) $(OBJS) -o $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(IFLAGS) -MMD -MP -c $< -o $@

tfm-gui: check-gtk-deps $(GUI_TARGET)

check-gtk-deps:
	@pkg-config --exists gtk4 || { \
		echo "gtk4 not found. Install it with your distro's package manager, e.g.:"; \
		echo "  Arch/Omarchy: sudo pacman -S gtk4"; \
		echo "  Debian/Ubuntu: sudo apt install libgtk-4-dev"; \
		echo "  Fedora: sudo dnf install gtk4-devel"; \
		exit 1; \
	}
	@pkg-config --exists libadwaita-1 || { \
		echo "libadwaita-1 not found. Install it with your distro's package manager, e.g.:"; \
		echo "  Arch/Omarchy: sudo pacman -S libadwaita"; \
		echo "  Debian/Ubuntu: sudo apt install libadwaita-1-dev"; \
		echo "  Fedora: sudo dnf install libadwaita-devel"; \
		exit 1; \
	}

$(GUI_TARGET): $(CORE_OBJS) $(GUI_OBJS) | $(BIN_DIR)
	$(CC) $(ALL_CFLAGS) $(CORE_OBJS) $(GUI_OBJS) $(GTK_LIBS) -o $(GUI_TARGET)


$(GUI_BUILD_DIR)/%.o: $(GUI_SRC_DIR)/%.c | $(GUI_BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(IFLAGS) $(GTK_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(GUI_BUILD_DIR):
	mkdir -p $(GUI_BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(INSTALL_DIR)/tfm
	@echo "Installed to $(DESTDIR)$(INSTALL_DIR)/tfm - 'tfm' should now run from anywhere."

install-gui: $(GUI_TARGET)
	install -Dm755 $(GUI_TARGET) $(DESTDIR)$(INSTALL_DIR)/tfm-gui
	@echo "Installed to $(DESTDIR)$(INSTALL_DIR)/tfm-gui - 'tfm-gui' should now run from anywhere."

uninstall:
	rm -f $(DESTDIR)$(INSTALL_DIR)/tfm
	@echo "Removed: $(DESTDIR)$(INSTALL_DIR)/tfm"

uninstall-gui:
	rm -f $(DESTDIR)$(INSTALL_DIR)/tfm-gui
	@echo "Removed: $(DESTDIR)$(INSTALL_DIR)/tfm-gui"

install-gui-theme-hook:
	install -Dm755 $(OMARCHY_HOOK_SRC) $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme
	@echo "Installed to $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme."

uninstall-gui-theme-hook:
	rm -f $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme
	@echo "Removed: $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme"

# Builds both binaries and checks --version/--help exit cleanly - a sanity
# check that the binary starts up and parses its own flags, not a
# substitute for unit-test below.
smoke-test: all
	@./$(TARGET) --version >/dev/null && echo "tfm --version: OK"
	@./$(TARGET) --help >/dev/null && echo "tfm --help: OK"
	@echo "Smoke test passed."

$(TEST_BUILD_DIR):
	mkdir -p $(TEST_BUILD_DIR)

# Each tests/test_*.c is its own self-contained binary (own main(), via
# tests/test.h) linked against the real $(CORE_OBJS) - so tests exercise
# the actual compiled fileops.c/tfm_common.c/etc, not a reimplementation.
# Same $(ALL_CFLAGS) $(IFLAGS) as the main pattern rule, so a test file
# that needs _GNU_SOURCE (e.g. for unshare()) picks up the same warning
# flags/std as the rest of the project automatically.
#
# input.c is TUI-only (not part of $(CORE_OBJS), which is the frontend-
# agnostic set shared with the GUI) - EXTRA_OBJS lets test_input link
# against it too without pulling it into every other test binary that
# doesn't need it. The extra prerequisite line below (rather than adding
# $(EXTRA_OBJS) to the pattern rule's own prerequisite list) is
# deliberate: a target-specific variable isn't reliably expanded while
# make computes a pattern rule's prerequisites, only within its recipe -
# a separate prerequisite-only rule for the same target is the standard,
# reliable way to add "build this first" without a recipe of its own.
$(TEST_BUILD_DIR)/test_input: EXTRA_OBJS = $(BUILD_DIR)/input.o
$(TEST_BUILD_DIR)/test_input: $(BUILD_DIR)/input.o

# panel.c is TUI-only too, and draws through screen.c (which in turn
# reads the terminal size via input.c) - all three are linked in, though
# the tests themselves never draw. --wrap=dir_list reroutes panel.o's
# dir_list() calls through a fault-injection shim in tests/test_panel.c
# (see the comment there) while dir.o's real implementation stays
# reachable as __real_dir_list.
$(TEST_BUILD_DIR)/test_panel: EXTRA_OBJS = $(BUILD_DIR)/panel.o $(BUILD_DIR)/screen.o $(BUILD_DIR)/input.o
$(TEST_BUILD_DIR)/test_panel: EXTRA_LDFLAGS = -Wl,--wrap=dir_list
$(TEST_BUILD_DIR)/test_panel: $(BUILD_DIR)/panel.o $(BUILD_DIR)/screen.o $(BUILD_DIR)/input.o

# omarchy_theme.c lives in the GUI tree but is plain libc (no GTK), so it
# is compiled straight into its test binary from source rather than via
# $(GUI_BUILD_DIR), whose objects are built with GTK's pkg-config flags -
# the test then runs on machines (and CI) without GTK installed.
$(TEST_BUILD_DIR)/test_omarchy_theme: EXTRA_OBJS = $(GUI_SRC_DIR)/omarchy_theme.c
$(TEST_BUILD_DIR)/test_omarchy_theme: $(GUI_SRC_DIR)/omarchy_theme.c $(GUI_SRC_DIR)/omarchy_theme.h

$(TEST_BUILD_DIR)/%: $(TEST_DIR)/%.c $(CORE_OBJS) | $(TEST_BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(IFLAGS) $< $(CORE_OBJS) $(EXTRA_OBJS) $(EXTRA_LDFLAGS) -o $@

# Builds and runs every tests/test_*.c binary; fails (non-zero exit) if
# any test in any of them fails.
unit-test: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "--- $$t ---"; \
		./$$t || exit 1; \
	done

test: unit-test smoke-test

# Slower, separate from unit-test/test: spawns the real compiled bin/tfm
# in a pseudo-terminal (forkpty()) to exercise signal/EOF/terminal-restore
# behavior no source-linked unit test can reach - see
# tests/pty_test_main.c's own header comment. Deliberately not named
# tests/test_*.c, so unit-test/test/asan-test never pick it up. -lutil
# for forkpty() portability to older glibc that hasn't merged it into
# libc yet (harmless where it's already merged).
PTY_TEST_SRC = $(TEST_DIR)/pty_test_main.c
PTY_TEST_BIN = $(TEST_BUILD_DIR)/pty_test_main

$(PTY_TEST_BIN): $(PTY_TEST_SRC) $(TARGET) | $(TEST_BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(IFLAGS) $(PTY_TEST_SRC) -lutil -o $@

test-pty: $(PTY_TEST_BIN)
	./$(PTY_TEST_BIN)

# Best-effort static analysis; skipped (not failed) if cppcheck isn't
# installed, so `make lint` is safe to run/CI-wire on any machine.
lint:
	@command -v cppcheck >/dev/null 2>&1 && \
		cppcheck --enable=warning,style --std=c11 -Iinclude --suppress=missingIncludeSystem \
			$(SRC_DIR) || echo "cppcheck not installed, skipping."

# Clang static analyzer over both shipped binaries. Skipped (not failed)
# if scan-build isn't installed, same as `lint`. --status-bugs makes any
# finding a non-zero exit, so CI can gate on it. `clean` first: scan-build
# only analyzes files it actually sees compiled, so leftover objects from
# a prior build would be silently skipped. tests/ is deliberately left
# out - the analyzer doesn't model __attribute__((cleanup)), which the
# test harness uses for env-var restore (tests/test_fs_helpers.h), so it
# reports false "leaks" there; `make asan-test`'s LeakSanitizer covers
# the tests at runtime instead.
scan:
	@if command -v scan-build >/dev/null 2>&1; then \
		$(MAKE) clean && scan-build --status-bugs $(MAKE) all tfm-gui; \
	else \
		echo "scan-build not installed, skipping."; \
	fi

# `make asan` builds the TUI with ASan+UBSan instead of the normal
# optimized/plain build - run bin/tfm under it manually.
asan: CFLAGS = -fsanitize=address,undefined -g -O0
asan: clean $(TARGET)

# Same idea as `make asan`, but for the unit-test binaries instead of the
# TUI - `clean` first so no plain object left over from a prior `make`/
# `make test` gets linked into an ASan binary.
asan-test: CFLAGS = -fsanitize=address,undefined -g -O0
asan-test: clean unit-test
