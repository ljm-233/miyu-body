CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
WL_CFLAGS := $(shell pkg-config --cflags wayland-client)
WL_LIBS   := $(shell pkg-config --libs wayland-client)

all: input-probe clicker test_region

gen/xdg-shell-protocol.c gen/xdg-shell-client.h: protocols/xdg-shell.xml
	mkdir -p gen
	wayland-scanner private-code $< gen/xdg-shell-protocol.c
	wayland-scanner client-header $< gen/xdg-shell-client.h

gen/layer-shell-protocol.c gen/layer-shell-client.h: protocols/wlr-layer-shell-unstable-v1.xml
	mkdir -p gen
	wayland-scanner private-code $< gen/layer-shell-protocol.c
	wayland-scanner client-header $< gen/layer-shell-client.h

gen/virtual-pointer-protocol.c gen/virtual-pointer-client.h: protocols/wlr-virtual-pointer-unstable-v1.xml
	mkdir -p gen
	wayland-scanner private-code $< gen/virtual-pointer-protocol.c
	wayland-scanner client-header $< gen/virtual-pointer-client.h

input-probe: input-probe.c alpha_region.c gen/layer-shell-protocol.c gen/xdg-shell-protocol.c
	$(CC) $(CFLAGS) $(WL_CFLAGS) -o $@ $^ $(WL_LIBS) -lm

clicker: clicker.c gen/virtual-pointer-protocol.c
	$(CC) $(CFLAGS) $(WL_CFLAGS) -o $@ $^ $(WL_LIBS)

test_region: test_region.c alpha_region.c
	$(CC) $(CFLAGS) $(WL_CFLAGS) -o $@ $^ $(WL_LIBS) -lm

clean:
	rm -f input-probe clicker test_region

.PHONY: all clean
