C := gcc
CFLAGS := -O2 -fstrict-aliasing -flto -fuse-ld=gold -ffunction-sections -fdata-sections -fno-stack-protector -fno-ident -fno-builtin -fno-plt -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions -fomit-frame-pointer -fvisibility=hidden -fno-math-errno -fmerge-all-constants -D_GNU_SOURCE
LDFLAGS := -Wl,-O1,-z,norelro,--gc-sections,--build-id=none,--as-needed,--icf=all,--hash-style=gnu,--discard-all,--strip-all
LIBS := -lX11 -lXinerama -lXfixes -lXtst -lm 

TARGET := xakard
SRC := xakar.c
CONFIG := config.py

PREFIX := /usr/local
BINDIR := $(PREFIX)/bin

AUTOSTART_DIR := $(HOME)/.trinity/Autostart
AUTOSTART_XAKAR := $(AUTOSTART_DIR)/xakar-daemon.desktop
AUTOSTART_XCAPE := $(AUTOSTART_DIR)/xcape-daemon.desktop
KHOTKEYSRC := $(HOME)/.trinity/share/config/khotkeysrc
KHOTKEYS_DIR := $(HOME)/.trinity/share/apps/khotkeys

.PHONY: all clean install uninstall install-deps

ifeq ($(shell id -u),0)
$(error [xakar] Do NOT run make with sudo! Just run 'make' as a normal user.)
endif

all: $(TARGET)

$(TARGET): $(SRC)
	@echo "[xakar] Compiling $^ -> $@"
	@$(C) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

install: $(TARGET)
	@if [ -x "$(BINDIR)/$(TARGET)" ]; then \
		echo "[xakar] $(TARGET) already installed at $(BINDIR)/$(TARGET), skipping"; \
	else \
		echo "[xakar] Installing $(TARGET) to $(BINDIR)"; \
		sudo install -m 755 $(TARGET) $(BINDIR)/$(TARGET); \
		echo "[xakar] Ensuring khotkeys directory exists"; \
		mkdir -p $(KHOTKEYS_DIR); \
		echo "[xakar] Running config.py to set up shortcuts and autostart"; \
		python3 $(CONFIG) --install; \
		chmod +x $(AUTOSTART_XAKAR) || true; \
		echo "[xakar] Updating Trinity khotkeys (setuzuna_xakar)"; \
	fi

uninstall:
	@if [ -x "$(BINDIR)/$(TARGET)" ]; then \
		killall xakard; \
		echo "[xakar] Removing $(TARGET) from $(BINDIR)"; \
		sudo rm -f $(BINDIR)/$(TARGET); \
		echo "[xakar] Removing autostart entries (if present)"; \
		rm -f $(AUTOSTART_XAKAR); \
	else \
		echo "[xakar] $(TARGET) is not installed in $(BINDIR), skipping"; \
	fi

clean:
	@echo "[xakar] Cleaning build artifacts"
	@rm -f $(TARGET)

install-deps:
	@echo "[xakar] Installing required development libraries (if missing)..."
	sudo apt update
	sudo apt install -y build-essential libx11-dev libxinerama-dev libxfixes-dev binutils-gold libxtst-dev libxi-dev
