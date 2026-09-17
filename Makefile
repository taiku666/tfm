CC = gcc
CSTD = -std=c11
WARN_FLAGS = -Wall -Wextra -Wpedantic

# CFLAGS is user/CI-overridable (e.g. `make CFLAGS=-O2` or
# `make CFLAGS='-fsanitize=address,undefined -g'`); the warning flags and
# language standard above are not part of it, so overriding CFLAGS can't
# accidentally drop them.
CFLAGS ?=
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

CORE_SRCS = config.c dir.c editor.c fileops.c shell.c tfm_common.c
CORE_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

GUI_SRCS = $(wildcard $(GUI_SRC_DIR)/*.c)
GUI_OBJS = $(patsubst $(GUI_SRC_DIR)/%.c,$(GUI_BUILD_DIR)/%.o,$(GUI_SRCS))

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

.PHONY: all clean install uninstall tfm-gui check-gtk-deps install-gui uninstall-gui \
        install-gui-theme-hook uninstall-gui-theme-hook test lint asan

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

# No automated test suite exists yet (see CODE_REVIEW.md's "Suggested test
# plan" section) - this is a smoke test, not a substitute for it: builds
# both binaries and checks --version/--help exit cleanly.
test: all
	@./$(TARGET) --version >/dev/null && echo "tfm --version: OK"
	@./$(TARGET) --help >/dev/null && echo "tfm --help: OK"
	@echo "Smoke test passed. No automated fileops/input test suite exists yet."

# Best-effort static analysis; skipped (not failed) if cppcheck isn't
# installed, so `make lint` is safe to run/CI-wire on any machine.
lint:
	@command -v cppcheck >/dev/null 2>&1 && \
		cppcheck --enable=warning,style --std=c11 -Iinclude --suppress=missingIncludeSystem \
			$(SRC_DIR) || echo "cppcheck not installed, skipping."

# `make asan` builds the TUI with ASan+UBSan instead of the normal
# optimized/plain build - run bin/tfm under it manually.
asan: CFLAGS = -fsanitize=address,undefined -g -O0
asan: clean $(TARGET)
