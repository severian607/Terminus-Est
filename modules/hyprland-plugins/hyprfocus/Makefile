# Local build against the installed Hyprland headers; rebuild with make -B after every Hyprland update.
CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2
PACKAGES := hyprland pixman-1 libdrm libinput libudev pangocairo wayland-server xkbcommon

.PHONY: all clean
.DELETE_ON_ERROR:
all: build/hyprfocus.so

build/hyprfocus.so: main.cpp globals.hpp Makefile
	mkdir -p build
	$(CXX) $(CXXFLAGS) -shared -fPIC -fno-gnu-unique -std=c++23 $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) main.cpp -o $@.tmp $(LDFLAGS)
	mv -f $@.tmp $@

clean:
	rm -rf build
