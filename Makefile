CXX      := g++
CC       := gcc
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Iprotocols -D_GNU_SOURCE
CFLAGS   := -O2 -Iprotocols
LIBS     := $(shell pkg-config --libs wayland-client libnl-3.0 libnl-genl-3.0)
CXXINC   := $(shell pkg-config --cflags wayland-client libnl-3.0 libnl-genl-3.0)

# xdg-shell.xml ships with the wayland-protocols package under a path that
# differs slightly between distros; ask pkg-config for the right one.
WAYLAND_PROTOCOLS_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)
XDG_SHELL_XML          := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

all: check_deps barcpp

check_deps:
	@pkg-config --exists wayland-client libnl-3.0 libnl-genl-3.0 wayland-protocols || \
		(echo "Error: Missing required dependencies." && \
		 echo "Please install: wayland, wayland-protocols, libnl, pkgconf" && \
		 echo "Arch Linux: sudo pacman -S --needed wayland wayland-protocols libnl pkgconf" && \
		 echo "Debian/Ubuntu: sudo apt install libwayland-dev wayland-protocols libnl-3-dev libnl-genl-3-dev pkg-config" && \
		 exit 1)

# --- wlr-layer-shell bindings (xml is vendored in protocols/) -------------
protocols/wlr-layer-shell-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner client-header $< $@
	# 'namespace' is a C++ keyword; the generated header uses it as a plain
	# C parameter name, which fails to compile under g++. Rename it.
	sed -i \
	    -e 's/const char \*namespace/const char *name_space/g' \
	    -e 's/, namespace)/, name_space)/g' \
	    $@

protocols/wlr-layer-shell-client-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner private-code $< $@

# --- xdg-shell bindings -----------------------------------------------------
# Needed purely because wlr-layer-shell's generated code references
# xdg_popup_interface (get_popup support) even though we never call it.
protocols/xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@

protocols/xdg-shell-client-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

protocols/wlr-layer-shell-client-protocol.o: protocols/wlr-layer-shell-client-protocol.c
	$(CC) $(CFLAGS) $(CXXINC) -c $< -o $@

protocols/xdg-shell-client-protocol.o: protocols/xdg-shell-client-protocol.c
	$(CC) $(CFLAGS) $(CXXINC) -c $< -o $@

src/main.o: src/main.cpp protocols/wlr-layer-shell-client-protocol.h protocols/xdg-shell-client-protocol.h
	$(CXX) $(CXXFLAGS) $(CXXINC) -c $< -o $@

barcpp: src/main.o protocols/wlr-layer-shell-client-protocol.o protocols/xdg-shell-client-protocol.o
	$(CXX) $^ -o $@ $(LIBS)

clean:
	rm -f barcpp src/main.o protocols/*.o protocols/*-client-protocol.h protocols/*-client-protocol.c

.PHONY: all clean
