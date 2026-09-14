CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11
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

GTK_PKGS = gtk4 libadwaita-1
GTK_CFLAGS := $(shell pkg-config --cflags $(GTK_PKGS) 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs $(GTK_PKGS) 2>/dev/null)

PREFIX ?= /usr/local
INSTALL_DIR = $(PREFIX)/bin

OMARCHY_HOOK_DIR = $(HOME)/.config/omarchy/hooks/theme-set.d
OMARCHY_HOOK_SRC = contrib/omarchy-hooks/tfm-gui-reload-theme

.PHONY: all clean install uninstall tfm-gui check-gtk-deps install-gui uninstall-gui \
        install-gui-theme-hook uninstall-gui-theme-hook

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

tfm-gui: check-gtk-deps $(GUI_TARGET)

check-gtk-deps:
	@pkg-config --exists gtk4 || { echo "gtk4 nicht gefunden. Installieren mit: sudo pacman -S gtk4"; exit 1; }
	@pkg-config --exists libadwaita-1 || { echo "libadwaita-1 nicht gefunden. Installieren mit: sudo pacman -S libadwaita"; exit 1; }

$(GUI_TARGET): $(CORE_OBJS) $(GUI_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CORE_OBJS) $(GUI_OBJS) $(GTK_LIBS) -o $(GUI_TARGET)


$(GUI_BUILD_DIR)/%.o: $(GUI_SRC_DIR)/%.c | $(GUI_BUILD_DIR)
	$(CC) -Wall -Wextra -std=c11 $(IFLAGS) $(GTK_CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(GUI_BUILD_DIR):
	mkdir -p $(GUI_BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

install: $(TARGET)
	install -Dm755 $(TARGET) $(INSTALL_DIR)/tfm
	@echo "Installiert nach $(INSTALL_DIR)/tfm - 'tfm' sollte jetzt ueberall aufrufbar sein."

install-gui: $(GUI_TARGET)
	install -Dm755 $(GUI_TARGET) $(INSTALL_DIR)/tfm-gui
	@echo "Installiert nach $(INSTALL_DIR)/tfm-gui - 'tfm-gui' sollte jetzt ueberall aufrufbar sein."

uninstall:
	rm -f $(INSTALL_DIR)/tfm
	@echo "Entfernt: $(INSTALL_DIR)/tfm"

uninstall-gui:
	rm -f $(INSTALL_DIR)/tfm-gui
	@echo "Entfernt: $(INSTALL_DIR)/tfm-gui"

install-gui-theme-hook:
	install -Dm755 $(OMARCHY_HOOK_SRC) $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme
	@echo "Installiert nach $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme."

uninstall-gui-theme-hook:
	rm -f $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme
	@echo "Entfernt: $(OMARCHY_HOOK_DIR)/tfm-gui-reload-theme"
