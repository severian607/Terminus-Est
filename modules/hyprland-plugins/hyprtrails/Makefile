# Local build against the installed Hyprland headers; rebuild with make -B after every Hyprland update.
CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2
PACKAGES := hyprland pixman-1 libdrm

SRC := main.cpp trail.cpp TrailPassElement.cpp

.PHONY: all clean
.DELETE_ON_ERROR:
all: build/hyprtrails.so

build/hyprtrails.so: $(SRC) $(wildcard *.hpp) Makefile
	mkdir -p build
	$(CXX) $(CXXFLAGS) -shared -fPIC -fno-gnu-unique -std=c++23 $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) $(SRC) -o $@.tmp $(LDFLAGS)
	mv -f $@.tmp $@

clean:
	rm -rf build
