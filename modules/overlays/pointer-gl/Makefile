# pointer-gl: GPU pointer trails (fire, lightning, plasma) as a click-through layer-shell overlay.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
B := build
SCAN := wayland-scanner
PKG := wayland-client wayland-egl egl glesv2
PROTOS := wlr-layer-shell-unstable-v1 xdg-output-unstable-v1 xdg-shell

all: $(B)/pointer-gl

$(B)/%-client-protocol.h: protocols/%.xml | $(B)
	$(SCAN) client-header $< $@
$(B)/%-protocol.c: protocols/%.xml | $(B)
	$(SCAN) private-code $< $@

GEN := $(foreach p,$(PROTOS),$(B)/$(p)-client-protocol.h $(B)/$(p)-protocol.c)

$(B):
	mkdir -p $(B)

$(B)/pointer-gl: pointer-gl.c $(GEN)
	$(CC) $(CFLAGS) -I$(B) $< $(foreach p,$(PROTOS),$(B)/$(p)-protocol.c) -o $@ $$(pkg-config --cflags --libs $(PKG)) -lm

clean:
	rm -rf $(B)

.PHONY: all clean
