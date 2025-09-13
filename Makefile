CXX := g++
CXXFLAGS := -Os -std=c++17 -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fomit-frame-pointer -ffunction-sections -fdata-sections -flto -s
LDFLAGS := -Wl,--gc-sections,--as-needed
LIBS := -lX11 -lXinerama

TARGET := xakard
SRC := xakar.cpp
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
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

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
		chmod +x $(AUTOSTART_XCAPE) || true; \
		echo "[xakar] Updating Trinity khotkeys (setuzuna_xakar)"; \
		/opt/trinity/lib/tdeconf_update_bin/khotkeys_update --id setuzuna_xakar || true; \
	fi

uninstall:
	@if [ -x "$(BINDIR)/$(TARGET)" ]; then \
		echo "[xakar] Removing $(TARGET) from $(BINDIR)"; \
		sudo rm -f $(BINDIR)/$(TARGET); \
		echo "[xakar] Removing autostart entries (if present)"; \
		rm -f $(AUTOSTART_XAKAR) $(AUTOSTART_XCAPE); \
		echo "[xakar] Cleaning up khotkeysrc"; \
		if [ -f $(KHOTKEYSRC) ]; then \
			sed -i 's/,setuzuna_xakar//g; s/setuzuna_xakar,//g; s/setuzuna_xakar//g' $(KHOTKEYSRC); \
			echo "[xakar] Removed setuzuna_xakar ID from $(KHOTKEYSRC)"; \
		else \
			echo "[xakar] $(KHOTKEYSRC) not found, skipping"; \
		fi; \
		echo "[xakar] Running config.py to uninstall shortcuts and creating empty config"; \
		python3 $(CONFIG) --uninstall; \
		echo "[xakar] Updating Trinity khotkeys"; \
		/opt/trinity/lib/tdeconf_update_bin/khotkeys_update --id setuzuna_empty || true; \
		echo "[xakar] Removing empty khotkeys"; \
		rm -f $(KHOTKEYS_DIR)/setuzuna_empty.khotkeys; \
		echo "[xakar] Removed setuzuna_empty ID from $(KHOTKEYSRC)"; \
		sed -i 's/,setuzuna_empty//g; s/setuzuna_empty,//g; s/setuzuna_empty//g' $(KHOTKEYSRC); \
		echo "[xakar] Removing custom khotkeys file (if present)"; \
		rm -f $(KHOTKEYS_DIR)/setuzuna_xakar.khotkeys; \
	else \
		echo "[xakar] $(TARGET) is not installed in $(BINDIR), skipping"; \
	fi

clean:
	@echo "[xakar] Cleaning build artifacts"
	@rm -f $(TARGET)

install-deps:
	@echo "[xakar] Installing required development libraries (if missing)..."
	sudo apt update
	sudo apt install -y build-essential libx11-dev libxinerama-dev
