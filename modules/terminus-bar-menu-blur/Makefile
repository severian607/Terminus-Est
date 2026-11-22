# Build locally with the same compiler and exact headers as the running Hyprland.
# Current target: Hyprland v0.57.2 11cbc6c577b84ffb1edf4df3fc5b6128c3ae6947 (first: v0.56.2),
# GCC 16.2.1 20260810. Hyprland's C++ ABI is not stable across updates.
# After compositor/compiler/dependency upgrades, review the hook and rebuild with
# make -B. Restart into the matching compositor before loading a rebuilt plugin.
# -MD tracks installed/system headers too; compiler or flag changes require -B.
# No installation, downloads, compositor reloads or automatic runtime compilation.
CXX = g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2 -g

TARGET := build/terminus-bar-menu-blur.so
OBJECT := build/main.o
PACKAGES := hyprland pixman-1

.PHONY: all clean
.DELETE_ON_ERROR:
all: $(TARGET)

# Hyprland requires RTTI (enabled by default) and C++23. Its installed headers
# define GLES32 themselves; no legacy wlroots/unstable-API defines are needed.
# GCC's GNU-unique statics prevent dlclose; disable them for safe unload/reload.
# Do not use -Bsymbolic: config-value registries must stay shared with Hyprland.
$(OBJECT): src/main.cpp Makefile | build
	$(PKG_CONFIG) --exists $(PACKAGES)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++23 -fPIC -fno-gnu-unique -Wall -Wextra \
		$$($(PKG_CONFIG) --cflags $(PACKAGES)) -MD -MP -MF build/main.d -c $< -o $@

# Hyprland symbols resolve from the compositor; this is a module, not an executable.
# Replace the inode only after linking, never overwrite a possibly mapped module.
$(TARGET): $(OBJECT)
	$(CXX) -shared $(LDFLAGS) $< $(LDLIBS) -o $@.tmp
	mv -f $@.tmp $@

build:
	mkdir -p $@

clean:
	rm -rf build

-include build/main.d
