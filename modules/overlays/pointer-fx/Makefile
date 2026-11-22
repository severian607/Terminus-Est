# pointer-fx: click ripple and pointer trail overlays (layer-shell client), plus sandbox-click for previews.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
B := build
SCAN := wayland-scanner
PKG := wayland-client cairo
PROTOS := wlr-layer-shell-unstable-v1 xdg-output-unstable-v1 xdg-shell

all: $(B)/pointer-fx $(B)/sandbox-click

$(B)/%-client-protocol.h: protocols/%.xml | $(B)
	$(SCAN) client-header $< $@
$(B)/%-protocol.c: protocols/%.xml | $(B)
	$(SCAN) private-code $< $@

GEN := $(foreach p,$(PROTOS),$(B)/$(p)-client-protocol.h $(B)/$(p)-protocol.c)

$(B):
	mkdir -p $(B)

$(B)/pointer-fx: pointer-fx.c $(GEN)
	$(CC) $(CFLAGS) -I$(B) $< $(foreach p,$(PROTOS),$(B)/$(p)-protocol.c) -o $@ $$(pkg-config --cflags --libs $(PKG)) -lm

$(B)/sandbox-click: sandbox-click.c $(B)/wlr-virtual-pointer-unstable-v1-client-protocol.h $(B)/wlr-virtual-pointer-unstable-v1-protocol.c
	$(CC) $(CFLAGS) -I$(B) $< $(B)/wlr-virtual-pointer-unstable-v1-protocol.c -o $@ $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -rf $(B)

.PHONY: all clean
