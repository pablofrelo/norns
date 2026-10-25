PROTO_DIR ?= /usr/share/river-protocols/stable
PROTOS    = river-window-management-v1 river-xkb-bindings-v1 river-layer-shell-v1
WL_PROTO_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WLPROTOS  = fractional-scale-v1 viewporter

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol \
          $(shell pkg-config --cflags wayland-client wayland-cursor xkbcommon fcft pixman-1 libsystemd libpng)
LDLIBS += $(shell pkg-config --libs wayland-client wayland-cursor fcft pixman-1 libsystemd libpng) -lm

HDRS = $(PROTOS:%=protocol/%-client-protocol.h) $(WLPROTOS:%=protocol/%-client-protocol.h)
SRCS = norns.c urd.c sensors.c shm.c anim.c deco.c tray.c nostromo.c verdandi.c $(PROTOS:%=protocol/%-protocol.c) \
       $(WLPROTOS:%=protocol/%-protocol.c)

norns: $(SRCS) $(HDRS) norns.h config.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

protocol/%-client-protocol.h: $(PROTO_DIR)/%.xml
	@mkdir -p protocol
	wayland-scanner client-header $< $@

protocol/%-protocol.c: $(PROTO_DIR)/%.xml
	@mkdir -p protocol
	wayland-scanner private-code $< $@

protocol/fractional-scale-v1-client-protocol.h: $(WL_PROTO_DIR)/staging/fractional-scale/fractional-scale-v1.xml
	@mkdir -p protocol
	wayland-scanner client-header $< $@

protocol/fractional-scale-v1-protocol.c: $(WL_PROTO_DIR)/staging/fractional-scale/fractional-scale-v1.xml
	@mkdir -p protocol
	wayland-scanner private-code $< $@

protocol/viewporter-client-protocol.h: $(WL_PROTO_DIR)/stable/viewporter/viewporter.xml
	@mkdir -p protocol
	wayland-scanner client-header $< $@

protocol/viewporter-protocol.c: $(WL_PROTO_DIR)/stable/viewporter/viewporter.xml
	@mkdir -p protocol
	wayland-scanner private-code $< $@

clean:
	rm -rf norns protocol

.PHONY: clean

install: norns
	install -Dm755 norns $(HOME)/.local/bin/norns
	install -Dm755 norns-ctl $(HOME)/.local/bin/norns-ctl
