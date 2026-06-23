CC ?= gcc
PKG_CONFIG ?= pkg-config

PREFIX ?= /opt/jx
BINDIR ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin
LIBEXECDIR ?= $(PREFIX)/libexec
LIBDIR ?= $(PREFIX)/lib
MODULEDIR ?= $(LIBDIR)/jx-sentinel-control
SRCDIR ?= $(PREFIX)/src/jx-sentinel
ETCDIR ?= $(PREFIX)/etc/jx-sentinel
SHAREDIR ?= $(PREFIX)/share/jx-sentinel-control
SYSTEMD_DIR ?= /etc/systemd/system
SYSTEMD_USER_DIR ?= /etc/xdg/systemd/user
APPLICATIONS_DIR ?= /usr/share/applications
AUTOSTART_DIR ?= /etc/xdg/autostart
POLKIT_DIR ?= /usr/share/polkit-1/actions
ICONDIR ?= /usr/share/icons/hicolor/256x256/apps

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
LDFLAGS ?=
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4 2>/dev/null)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk4 2>/dev/null)
WEBKIT_CFLAGS := $(shell $(PKG_CONFIG) --cflags webkitgtk-6.0 javascriptcoregtk-6.0 2>/dev/null)
WEBKIT_LIBS := $(shell $(PKG_CONFIG) --libs webkitgtk-6.0 javascriptcoregtk-6.0 2>/dev/null)
GUI_CFLAGS := $(filter-out -Wpedantic,$(CFLAGS)) -Wno-deprecated-declarations

DAEMON_BIN := jx-sentinel
GUARD_BIN := jx-sentinel-guard
AGENT_BIN := jx-permission-agent
APPLET_BIN := jx-sentinel-applet
CONTROL_BIN := jx-sentinel-control
HELPER_BIN := jx-sentinel-helper
PROMPT_HELPER := jx-guard-zenity-prompt

DAEMON_SOURCES := jx_sentinel.c jx_notify.c
DAEMON_HEADERS := jx_sentinel.h
GUARD_SOURCES := jx_sentinel_guard.c
GUARD_HEADERS := jx_guard_protocol.h
AGENT_SOURCES := jx_permission_agent.c
AGENT_HEADERS := jx_guard_protocol.h
APPLET_SOURCES := jx_sentinel_applet.c
APPLET_HEADERS := jx_sentinel_control.h
CONTROL_SOURCES := jx_sentinel_control.c jx_splash.c jx_module_loader.c
CONTROL_HEADERS := jx_sentinel_control.h jx_splash.h jx_module.h jx_module_loader.h
HELPER_SOURCES := jx_sentinel_helper.c
MODULE_SOURCES := \
	modules/jx_module_config.c \
	modules/jx_module_service.c \
	modules/jx_module_logs.c \
	modules/jx_module_notify.c \
	modules/jx_module_graph.c \
	modules/jx_module_diagnostics.c
MODULE_TARGETS := \
	libjx_config.so \
	libjx_service.so \
	libjx_logs.so \
	libjx_notify.so \
	libjx_graph.so \
	libjx_diagnostics.so

.PHONY: all daemon guard agent applet control helper modules clean install install-service install-control install-guard-config uninstall help

all: daemon guard agent applet control helper modules

daemon: $(DAEMON_BIN)

guard: $(GUARD_BIN)

agent: $(AGENT_BIN)

applet: $(APPLET_BIN)

control: $(CONTROL_BIN)

helper: $(HELPER_BIN)

modules: $(MODULE_TARGETS)

$(DAEMON_BIN): $(DAEMON_SOURCES) $(DAEMON_HEADERS)
	$(CC) $(CFLAGS) $(DAEMON_SOURCES) -o $@ $(LDFLAGS)

$(GUARD_BIN): $(GUARD_SOURCES) $(GUARD_HEADERS)
	$(CC) $(CFLAGS) $(GUARD_SOURCES) -o $@ -pthread $(LDFLAGS)

$(AGENT_BIN): $(AGENT_SOURCES) $(AGENT_HEADERS)
	@if ! command -v "$(PKG_CONFIG)" >/dev/null 2>&1 || ! $(PKG_CONFIG) --exists gtk4; then \
		echo "GTK4 development files are required: sudo apt install -y pkg-config libgtk-4-dev"; \
		exit 1; \
	fi
	$(CC) $(GUI_CFLAGS) $(GTK_CFLAGS) $(AGENT_SOURCES) -o $@ $(GTK_LIBS) $(LDFLAGS)

$(APPLET_BIN): $(APPLET_SOURCES) $(APPLET_HEADERS)
	@if ! command -v "$(PKG_CONFIG)" >/dev/null 2>&1 || ! $(PKG_CONFIG) --exists gtk4; then \
		echo "GTK4 development files are required: sudo apt install -y pkg-config libgtk-4-dev"; \
		exit 1; \
	fi
	$(CC) $(GUI_CFLAGS) $(GTK_CFLAGS) $(APPLET_SOURCES) -o $@ $(GTK_LIBS) $(LDFLAGS)

$(CONTROL_BIN): $(CONTROL_SOURCES) $(CONTROL_HEADERS)
	@if ! command -v "$(PKG_CONFIG)" >/dev/null 2>&1 || ! $(PKG_CONFIG) --exists gtk4; then \
		echo "GTK4 development files are required: sudo apt install -y pkg-config libgtk-4-dev"; \
		exit 1; \
	fi
	@if ! $(PKG_CONFIG) --exists webkitgtk-6.0 javascriptcoregtk-6.0; then \
		echo "WebKitGTK and JavaScriptCoreGTK development files are required: sudo apt install -y libwebkitgtk-6.0-dev libjavascriptcoregtk-6.0-dev"; \
		exit 1; \
	fi
	$(CC) $(GUI_CFLAGS) $(GTK_CFLAGS) $(WEBKIT_CFLAGS) $(CONTROL_SOURCES) -o $@ $(GTK_LIBS) $(WEBKIT_LIBS) -ldl $(LDFLAGS)

$(HELPER_BIN): $(HELPER_SOURCES)
	$(CC) $(CFLAGS) $(HELPER_SOURCES) -o $@ $(LDFLAGS)

libjx_config.so: modules/jx_module_config.c jx_module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

libjx_service.so: modules/jx_module_service.c jx_module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

libjx_logs.so: modules/jx_module_logs.c jx_module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

libjx_notify.so: modules/jx_module_notify.c jx_module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

libjx_graph.so: modules/jx_module_graph.c jx_module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

libjx_diagnostics.so: modules/jx_module_diagnostics.c jx_module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@

install: all
	install -d "$(DESTDIR)$(PREFIX)" "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(SBINDIR)" "$(DESTDIR)$(LIBEXECDIR)" "$(DESTDIR)$(MODULEDIR)" "$(DESTDIR)$(SRCDIR)" "$(DESTDIR)$(ETCDIR)" "$(DESTDIR)$(SHAREDIR)" "$(DESTDIR)$(APPLICATIONS_DIR)" "$(DESTDIR)$(AUTOSTART_DIR)" "$(DESTDIR)$(SYSTEMD_USER_DIR)" "$(DESTDIR)$(POLKIT_DIR)" "$(DESTDIR)$(ICONDIR)" "$(DESTDIR)$(PREFIX)/var/lib/jx-sentinel"
	install -m 0755 "$(DAEMON_BIN)" "$(DESTDIR)$(SBINDIR)/$(DAEMON_BIN)"
	install -m 0755 "$(GUARD_BIN)" "$(DESTDIR)$(SBINDIR)/$(GUARD_BIN)"
	install -m 0755 "$(AGENT_BIN)" "$(DESTDIR)$(BINDIR)/$(AGENT_BIN)"
	install -m 0755 "$(APPLET_BIN)" "$(DESTDIR)$(BINDIR)/$(APPLET_BIN)"
	install -m 0755 "$(CONTROL_BIN)" "$(DESTDIR)$(BINDIR)/$(CONTROL_BIN)"
	install -m 0755 "$(HELPER_BIN)" "$(DESTDIR)$(LIBEXECDIR)/$(HELPER_BIN)"
	install -m 0755 "$(PROMPT_HELPER)" "$(DESTDIR)$(LIBEXECDIR)/$(PROMPT_HELPER)"
	install -m 0644 $(MODULE_TARGETS) "$(DESTDIR)$(MODULEDIR)/"
	@if [ ! -f "$(DESTDIR)$(ETCDIR)/jx-sentinel.conf" ]; then \
		install -m 0644 jx-sentinel.conf "$(DESTDIR)$(ETCDIR)/jx-sentinel.conf"; \
	else \
		echo "Keeping existing $(DESTDIR)$(ETCDIR)/jx-sentinel.conf"; \
	fi
	@if [ ! -f "$(DESTDIR)$(ETCDIR)/guard.conf" ]; then \
		install -m 0644 guard.conf "$(DESTDIR)$(ETCDIR)/guard.conf"; \
	else \
		echo "Keeping existing $(DESTDIR)$(ETCDIR)/guard.conf"; \
	fi
	install -m 0644 assets/banner.png "$(DESTDIR)$(SHAREDIR)/banner.png"
	install -m 0644 assets/logo-all-theme.png "$(DESTDIR)$(SHAREDIR)/logo-all-theme.png"
	install -m 0644 assets/logo-all-theme.png "$(DESTDIR)$(ICONDIR)/org.jx.sentinel.control.png"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/jx-sentinel-control.desktop"
	install -m 0644 jx-sentinel-control.desktop "$(DESTDIR)$(APPLICATIONS_DIR)/org.jx.sentinel.control.desktop"
	install -m 0644 jx-permission-agent.desktop "$(DESTDIR)$(AUTOSTART_DIR)/jx-permission-agent.desktop"
	install -m 0644 jx-permission-agent.service "$(DESTDIR)$(SYSTEMD_USER_DIR)/jx-permission-agent.service"
	install -m 0644 jx-sentinel-helper.policy "$(DESTDIR)$(POLKIT_DIR)/jx-sentinel-helper.policy"
	install -m 0644 $(sort $(DAEMON_SOURCES) $(DAEMON_HEADERS) $(GUARD_SOURCES) $(GUARD_HEADERS) $(AGENT_SOURCES) $(AGENT_HEADERS) $(APPLET_SOURCES) $(APPLET_HEADERS) $(CONTROL_SOURCES) $(CONTROL_HEADERS) $(HELPER_SOURCES) $(MODULE_SOURCES) Makefile jx-sentinel.service jx-sentinel-guard.service jx-sentinel.conf guard.conf jx-sentinel-control.desktop jx-permission-agent.desktop jx-permission-agent.service jx-sentinel-helper.policy $(PROMPT_HELPER) JX-Sentinel-Guard.md) "$(DESTDIR)$(SRCDIR)/"

install-service:
	install -d "$(DESTDIR)$(SYSTEMD_DIR)"
	install -m 0644 jx-sentinel.service "$(DESTDIR)$(SYSTEMD_DIR)/jx-sentinel.service"
	install -m 0644 jx-sentinel-guard.service "$(DESTDIR)$(SYSTEMD_DIR)/jx-sentinel-guard.service"

install-guard-config:
	install -d "$(DESTDIR)$(ETCDIR)"
	install -m 0644 guard.conf "$(DESTDIR)$(ETCDIR)/guard.conf"

install-control: $(CONTROL_BIN) $(AGENT_BIN) $(APPLET_BIN) $(HELPER_BIN) modules
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBEXECDIR)" "$(DESTDIR)$(MODULEDIR)" "$(DESTDIR)$(SHAREDIR)" "$(DESTDIR)$(APPLICATIONS_DIR)" "$(DESTDIR)$(AUTOSTART_DIR)" "$(DESTDIR)$(SYSTEMD_USER_DIR)" "$(DESTDIR)$(POLKIT_DIR)" "$(DESTDIR)$(ICONDIR)"
	install -m 0755 "$(CONTROL_BIN)" "$(DESTDIR)$(BINDIR)/$(CONTROL_BIN)"
	install -m 0755 "$(AGENT_BIN)" "$(DESTDIR)$(BINDIR)/$(AGENT_BIN)"
	install -m 0755 "$(APPLET_BIN)" "$(DESTDIR)$(BINDIR)/$(APPLET_BIN)"
	install -m 0755 "$(HELPER_BIN)" "$(DESTDIR)$(LIBEXECDIR)/$(HELPER_BIN)"
	install -m 0755 "$(PROMPT_HELPER)" "$(DESTDIR)$(LIBEXECDIR)/$(PROMPT_HELPER)"
	install -m 0644 $(MODULE_TARGETS) "$(DESTDIR)$(MODULEDIR)/"
	install -m 0644 assets/banner.png "$(DESTDIR)$(SHAREDIR)/banner.png"
	install -m 0644 assets/logo-all-theme.png "$(DESTDIR)$(SHAREDIR)/logo-all-theme.png"
	install -m 0644 assets/logo-all-theme.png "$(DESTDIR)$(ICONDIR)/org.jx.sentinel.control.png"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/jx-sentinel-control.desktop"
	install -m 0644 jx-sentinel-control.desktop "$(DESTDIR)$(APPLICATIONS_DIR)/org.jx.sentinel.control.desktop"
	install -m 0644 jx-permission-agent.desktop "$(DESTDIR)$(AUTOSTART_DIR)/jx-permission-agent.desktop"
	install -m 0644 jx-permission-agent.service "$(DESTDIR)$(SYSTEMD_USER_DIR)/jx-permission-agent.service"
	install -m 0644 jx-sentinel-helper.policy "$(DESTDIR)$(POLKIT_DIR)/jx-sentinel-helper.policy"

uninstall:
	rm -f "$(DESTDIR)$(SBINDIR)/$(DAEMON_BIN)"
	rm -f "$(DESTDIR)$(SBINDIR)/$(GUARD_BIN)"
	rm -f "$(DESTDIR)$(BINDIR)/$(AGENT_BIN)"
	rm -f "$(DESTDIR)$(BINDIR)/$(APPLET_BIN)"
	rm -f "$(DESTDIR)$(BINDIR)/$(CONTROL_BIN)"
	rm -f "$(DESTDIR)$(LIBEXECDIR)/$(HELPER_BIN)"
	rm -f "$(DESTDIR)$(LIBEXECDIR)/$(PROMPT_HELPER)"
	rm -f "$(DESTDIR)$(MODULEDIR)/libjx_config.so" "$(DESTDIR)$(MODULEDIR)/libjx_service.so" "$(DESTDIR)$(MODULEDIR)/libjx_logs.so" "$(DESTDIR)$(MODULEDIR)/libjx_notify.so" "$(DESTDIR)$(MODULEDIR)/libjx_graph.so" "$(DESTDIR)$(MODULEDIR)/libjx_diagnostics.so"
	rm -f "$(DESTDIR)$(SYSTEMD_DIR)/jx-sentinel.service"
	rm -f "$(DESTDIR)$(SYSTEMD_DIR)/jx-sentinel-guard.service"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/jx-sentinel-control.desktop"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/org.jx.sentinel.control.desktop"
	rm -f "$(DESTDIR)$(ICONDIR)/org.jx.sentinel.control.png"
	rm -f "$(DESTDIR)$(AUTOSTART_DIR)/jx-permission-agent.desktop"
	rm -f "$(DESTDIR)$(SYSTEMD_USER_DIR)/jx-permission-agent.service"
	rm -f "$(DESTDIR)$(POLKIT_DIR)/jx-sentinel-helper.policy"

clean:
	rm -f "$(DAEMON_BIN)" "$(GUARD_BIN)" "$(AGENT_BIN)" "$(APPLET_BIN)" "$(CONTROL_BIN)" "$(HELPER_BIN)" $(MODULE_TARGETS) *.o

help:
	@printf '%s\n' \
		'Build dependencies: sudo apt update && sudo apt install -y build-essential pkg-config libgtk-4-dev libadwaita-1-dev libpolkit-gobject-1-dev libnotify-bin zenity -y' \
		'Graph dependencies: sudo apt install -y libwebkitgtk-6.0-dev libjavascriptcoregtk-6.0-dev' \
		'Build all:          make' \
		'Build guard:        make guard agent applet' \
		'Build daemon only:  make daemon' \
		'Build control app:  make control helper modules' \
		'Create dirs:        sudo install -d /opt/jx /opt/jx/bin /opt/jx/sbin /opt/jx/libexec /opt/jx/lib/jx-sentinel-control /opt/jx/src/jx-sentinel /opt/jx/etc/jx-sentinel /opt/jx/share/jx-sentinel-control /opt/jx/var/lib/jx-sentinel' \
		'Install all:        sudo make install install-service' \
		'Update guard conf:  sudo make install-guard-config' \
		'Reload desktop DB:  sudo update-desktop-database /usr/share/applications || true' \
		'Reload systemd:     sudo systemctl daemon-reload' \
		'Enable service:     sudo systemctl enable jx-sentinel.service' \
		'Enable guard:       sudo systemctl enable --now jx-sentinel-guard.service' \
		'Enable agent:       systemctl --user enable --now jx-permission-agent.service' \
		'Start service:      sudo systemctl start jx-sentinel.service' \
		'Start agent:        /opt/jx/bin/jx-permission-agent' \
		'Open GUI:           /opt/jx/bin/jx-sentinel-control' \
		'Check status:       systemctl status jx-sentinel.service' \
		'Follow logs:        journalctl -u jx-sentinel.service -f' \
		'Follow guard logs:  journalctl -u jx-sentinel-guard.service -f' \
		'Uninstall:          sudo make uninstall && sudo systemctl daemon-reload' \
		'Test directory:     sudo install -d /opt/jx/test-dir' \
		'Matching file:      sudo touch /opt/jx/test.conf' \
		'Non-matching file:  sudo touch /opt/jx/test.txt' \
		'GUI testing:        confirm splash/banner, one-by-one module loads, optional failures continue, required failures stop, status/config/logs/notifications/graph/timeline update.' \
		'Expected behavior:  .conf and directory creations are logged; .txt is filtered when --ext filters are active.' \
		'Limitation:         This implementation watches direct children only. Deep recursive monitoring should use filesystem marks with path-prefix filtering, or auditd rules for full recursive forensic tracking.'
