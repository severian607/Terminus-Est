# Packaging TerminusBar

Notes for packaging this local source tree. Build instructions live in
[BUILDING.md](BUILDING.md); the project overview is in [README.md](README.md).

## Package description

Use this exact short description for package metadata (`pkgdesc`, `Summary`,
AppStream, etc.):

> A sleek, customizable desktop shell crafted for Wayland.

Do not substitute shorter or alternate blurbs (“lightweight Wayland bar”,
“status bar”, ...). TerminusBar is a full desktop shell (bars, dock, launcher,
notifications, lock screen, wallpaper, settings, ...), not a bar-only tool.

## Availability

TerminusBar is maintained as a local source build. Upstream distribution packages are
not interchangeable with this executable, configuration root or plugin API.

## Identity

| | |
|---|---|
| Name | `terminusbar` |
| Upstream attribution | https://github.com/noctalia-dev/noctalia |
| License | MIT ([LICENSE](LICENSE)); also see vendored licenses under `third_party/` for SPDX completeness |
| Version | [`VERSION`](VERSION), read by Meson `project(... version: files('VERSION'))` |
| Binary | `terminusbar` |
| Desktop entry | `dev.terminus.TerminusBar.desktop` |
| Icon | `terminusbar` (`share/icons/hicolor/scalable/apps/terminusbar.svg`) |

## Maintenance Policy

Releases strive to follow [semantic versioning](https://semver.org).
The latest version is the only actively maintained version.
No attempts are made to achieve overlapping lifecycles of major or minor versions.
Bug reports are required to be reproducible on the latest version.

## Architectures / libc

First-class targets:

- `x86_64` (amd64)
- `aarch64` (arm64)

Linux only. There is no intentional arch-specific code path; other little-endian
Linux arches may build if dependencies are available, but they are not
first-class supported.

**musl**: known to build (e.g. Alpine / Void musl). Not first-class tested.
`jemalloc` is glibc-only, on musl leave `-Djemalloc=disabled` (or `auto`, which
skips it).

## Compilers / linkers

C++23 is required.

| Toolchain | Minimum | Notes |
|---|---|---|
| GCC + libstdc++ | **GCC 13+** | Default on most distros |
| Clang + libstdc++ or libc++ | **Clang 16+** | libc++ needs Meson’s experimental-library flags (already wired in `meson.build`) |

Debian 12 “bookworm” needs a newer toolchain than the default `g++` (e.g.
`g++-13` with `CXX=g++-13`).

No special linker is required. The default linker from the compiler driver is
fine (**GNU ld** / gold, **lld**, **mold** via `-fuse-ld=` all work). Release
builds pass `-Wl,--gc-sections` and `-Wl,--as-needed`, which those linkers
support.

## Build

- Build system: [Meson](https://mesonbuild.com/) + Ninja. The repo’s
  [`justfile`](justfile) is convenience only; packaging can call Meson
  directly.
- Language / toolchain: see [Compilers / linkers](#compilers--linkers).
- Recommended: `meson setup build --buildtype=release`, then compile/install.
- Do **not** enable `-Dnative_optimizations=true` for distro packages (CPU-local
  codegen; not portable).
- Tests: `-Dtests=disabled` (or leave `auto`, which skips tests for release).
- `jemalloc`: recommended on glibc; Meson feature option `-Djemalloc=auto|enabled|disabled`.
  Only used on glibc builds.

Prefix/datadir are baked into the binary via `TERMINUSBAR_INSTALL_PREFIX` /
`TERMINUSBAR_INSTALL_DATADIR`. Install with the same prefix you configured.

### Installed layout

```text
<prefix>/bin/terminusbar
<prefix>/share/terminusbar/assets/...
<prefix>/share/applications/dev.terminus.TerminusBar.desktop
<prefix>/share/icons/hicolor/scalable/apps/terminusbar.svg
```

The shipped `assets/` tree is **required at runtime**. Shipping only the binary
breaks fonts, translations, templates, glyphs, and sounds.

### Runtime asset lookup

Besides the prefix layout above, portable bundle layouts are supported:

```text
bundle/
  terminusbar
  assets/
```

```text
bundle/
  bin/terminusbar
  share/terminusbar/assets/
```

TerminusBar resolves the asset root in this order:

1. `TERMINUSBAR_ASSETS_DIR` (a warning is logged if it is set but invalid)
2. `assets/` next to the executable
3. `assets/` one level above the executable (this is how a `build-debug/`
   binary finds the source tree's `assets/`)
4. `../<datadir>/terminusbar/assets` relative to the executable, when the
   configured Meson `datadir` is relative
5. `../share/terminusbar/assets` relative to the executable
6. the compiled install path from Meson (`<prefix>/<datadir>/terminusbar/assets`)

A candidate is only accepted if it contains `emoji.json`,
`fonts/terminusbar-tabler.ttf`, `templates/builtin.toml`, and
`translations/en.json`. If none qualifies, TerminusBar logs a warning and uses the
compiled install path.

Not shipped (don’t look for them in the install): AppStream / metainfo XML, man
pages, systemd units.

### Shell completions

Static completion files are not shipped. Generate them from the freshly built
binary and install them in the distribution's completion directories:

```text
./build-<mode>/terminusbar completions bash  → share/bash-completion/completions/terminusbar
./build-<mode>/terminusbar completions zsh   → share/zsh/site-functions/_terminusbar
./build-<mode>/terminusbar completions fish  → share/fish/vendor_completions.d/terminusbar.fish
```

This is intentionally a packaging step rather than a Meson install rule:
executing the target binary during installation would break cross-compilation.

## Dependencies

No Qt or GTK. UI is Wayland + OpenGL ES (EGL/GLES, or `libepoxy` as a fallback
when separate EGL/GLES packages are missing).

### Build-time (required)

Canonical list is the `dependency(...)` / header checks in
[`meson.build`](meson.build). Distro package names differ; [BUILDING.md](BUILDING.md)
has copy-paste install lines for common distros.

Notes packagers hit often:

- **stb** must provide `stb/stb_image_resize2.h` (and `stb/stb_image_write.h`).
  Older packages that only ship `stb_image_resize` are not enough. Meson fails
  the configure check if `stb_image_resize2` is missing.
- Meson requires **WirePlumber 0.5** (`wireplumber-0.5` pkg-config). 0.4 is not
  enough.

### Vendored (no system package)

Shipped under `third_party/`: Wuffs, Luau, fzy, Material Color Utilities.
Each carries its own license file beside the code.

### Runtime

| Dependency | Role |
|---|---|
| PipeWire **daemon** (+ WirePlumber 0.5) | Audio, volume OSD, privacy indicators, spectrum. Libraries alone are not enough; without a running daemon those features stay off. |
| PAM (`login` service by default) | Lock screen authentication |
| Fontconfig / fonts | Text rendering (users still need usable fonts installed) |
| `git` | Plugin git sources / auto-update invoke `git` on `PATH` |
| `upower` | Optional: battery / power devices |
| `ddcutil` | Optional: external monitor brightness |
| Secret Service provider | Optional but recommended for credential / encrypted-state persistence (GNOME Keyring, KWallet, KeePassXC, ...). `libsecret` is only the client library; without a session provider those features cannot persist secrets. |
| `sound-theme-freedesktop` | Shell sounds |

## Startup and IPC

No systemd user unit is shipped. Typical startup is compositor autostart or the
desktop entry (`terminusbar --daemon`). Control a running instance with
`terminusbar msg ...` (Unix socket under `XDG_RUNTIME_DIR`).

Keep the `.desktop` daemon Exec unless you intentionally package a different
session integration. Prefer the canonical description above for `Comment=` /
AppStream summary as well.

## Session conflicts

On non-Plasma sessions TerminusBar provides and registers:

- `org.freedesktop.Notifications`
- `org.kde.StatusNotifierWatcher` (system tray host)

Do not run it alongside another notification daemon or StatusNotifier host
(mako, dunst, swaync, waybar-as-host, ...) unless those are disabled. On Plasma,
TerminusBar integrates with Plasma's notification / tray paths instead of claiming
the freedesktop Notifications name.

## User data paths

| Kind | Default |
|---|---|
| Config | `$XDG_CONFIG_HOME/terminus-est/terminus-bar` (`~/.config/terminus-est/terminus-bar`), e.g. `config.toml` |
| State | `$XDG_STATE_HOME/terminusbar` (`~/.local/state/terminusbar`), e.g. `settings.toml`, caches |
| Data | `$XDG_DATA_HOME/terminusbar` (`~/.local/share/terminusbar`) |
| Logs | `$XDG_CACHE_HOME/terminusbar` (`~/.cache/terminusbar`) |

Override bases with `TERMINUSBAR_CONFIG_HOME` (appends `/terminus-est/terminus-bar`),
`TERMINUSBAR_STATE_HOME` and `TERMINUSBAR_DATA_HOME` (append `/terminusbar`).

## What TerminusBar is not

- Not a compositor, display manager, or login-screen configuration owner.
- Not a replacement for file managers, screen casting, or drive mounting.
- Compositor support varies according to available protocols and IPC.

## Versioning

Prefer packaging **tagged releases** rather than random `main` snapshots unless
you maintain a `-git` / nightly package on purpose.

