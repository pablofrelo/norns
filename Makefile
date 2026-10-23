PROTO_DIR ?= /usr/share/river-protocols/stable
PROTOS    = river-window-management-v1 river-xkb-bindings-v1 river-layer-shell-v1

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol \
          $(shell pkg-config --cflags wayland-client wayland-cursor xkbcommon fcft pixman-1)
LDLIBS += $(shell pkg-config --libs wayland-client wayland-cursor fcft pixman-1)

HDRS = $(PROTOS:%=protocol/%-client-protocol.h)
SRCS = norns.c urd.c $(PROTOS:%=protocol/%-protocol.c)

norns: $(SRCS) $(HDRS) norns.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

protocol/%-client-protocol.h: $(PROTO_DIR)/%.xml
	@mkdir -p protocol
	wayland-scanner client-header $< $@

protocol/%-protocol.c: $(PROTO_DIR)/%.xml
	@mkdir -p protocol
	wayland-scanner private-code $< $@

clean:
	rm -rf norns protocol

.PHONY: clean
