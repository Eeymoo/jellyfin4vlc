# jellyfin4vlc - VLC 3.0 interface plugin for Jellyfin
#
# Requires:
#   - VLC 3.0 development files (vlc-plugin pkg-config module, e.g. vlc-plugin-dev / vlc-devel)
#   - libcurl development files (libcurl4-openssl-dev / libcurl-devel)
#
# Install target copies the plugin into VLC's plugin directory and runs
# `vlc-cache-gen` if available.

CC      ?= cc
CFLAGS  ?= -O2

# vlc-plugin flags via pkg-config (set PKG_CONFIG_PATH / PKG_CONFIG_SYSROOT_DIR
# when building against an extracted .deb tree); libcurl linked directly to
# avoid its long Requires.private chain.
VLC_CFLAGS := $(shell pkg-config --cflags vlc-plugin)
VLC_LIBS   := $(shell pkg-config --libs vlc-plugin)
CURL_CFLAGS ?=
CURL_LIBS  ?= -lcurl

# __USE_MINGW_ANSI_STDIO: MinGW needs it for C99 printf formats (%zu);
# harmless no-op on Linux/macOS.
CFLAGS  += -std=gnu11 -Wall -Wextra -fPIC -D_GNU_SOURCE -D__USE_MINGW_ANSI_STDIO=1 \
           -Isrc $(VLC_CFLAGS) $(CURL_CFLAGS) -DMODULE_STRING='"jellyfin"'
# Plugins must NOT link libvlccore: symbols (vlc_*, module entry) are
# resolved by the host VLC process at dlopen time.
LDLIBS   = $(CURL_LIBS)

ifeq ($(shell pkg-config --exists vlc-plugin || echo no),no)
$(warning vlc-plugin pkg-config module not found - install vlc-plugin-dev)
endif

SRC = src/jellyfin.c src/jf_api.c src/jf_http.c src/cJSON.c
OBJ = $(SRC:.c=.o)

# Windows (MSYS2/MinGW) VLC plugins are .dll; elsewhere .so
ifeq ($(OS),Windows_NT)
PLUGIN := libjellyfin_plugin.dll
# PE linker refuses undefined symbols: link the libvlccore import library
# (shipped by mingw vlc packages); at runtime the symbols resolve against
# the libvlccore.dll already loaded by the host VLC process.
LDLIBS += $(VLC_LIBS)
else
PLUGIN := libjellyfin_plugin.so
endif

VLC_LIBDIR = $(shell pkg-config --variable=libdir vlc-plugin 2>/dev/null)
PLUGIN_DIR ?= $(VLC_LIBDIR)/plugins/interface

all: $(PLUGIN)

$(PLUGIN): $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Help cross-compiling against a VLC source tree (Windows): pass e.g.
#   make VLC_SRC=/c/src/vlc-3.0.20
ifdef VLC_SRC
CFLAGS += -I$(VLC_SRC)/include -I$(VLC_SRC)/include/vlc_plugins
endif

install: $(PLUGIN)
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 0755 $(PLUGIN) $(DESTDIR)$(PLUGIN_DIR)
	-cache-gen $(DESTDIR)$(VLC_LIBDIR)/plugins 2>/dev/null || \
	 vlc-cache-gen $(DESTDIR)$(VLC_LIBDIR)/plugins 2>/dev/null || true

clean:
	rm -f $(OBJ) $(PLUGIN) tests/test_path

# Host unit tests (no VLC headers required; uses a stub libcurl)
test: tests/test_path.c tests/stub/curl_stub.c src/jf_api.c src/jf_http.c src/cJSON.c
	$(CC) -std=gnu11 -D_GNU_SOURCE -Wall -Wextra \
	      -I tests/stub -I src $^ -o tests/test_path
	./tests/test_path

.PHONY: all install clean test
