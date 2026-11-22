# Terminus-Bar

Terminus-Bar is Terminus-Est's native Wayland desktop shell: bars, widgets, dock, launcher,
control center, notifications, wallpaper, lock screen, session actions, clipboard
history, OSDs, tray integration, and desktop widgets. It uses C++23, Wayland and
OpenGL ES rather than a Qt or GTK shell.

## Build and packaging

Build this source tree using [BUILDING.md](BUILDING.md). The executable and Meson
target are `terminusbar`; the desktop application ID is `dev.terminus.TerminusBar`.
Distribution packaging details are in [PACKAGING.md](PACKAGING.md). This local
project does not claim an upstream package repository or public release service.

## Configuration and runtime

[example.toml](example.toml) documents the configuration. The shell preserves its
native layout, controls, palette colors and hot-reload behavior.

- Configuration: `~/.config/terminus-est/terminus-bar`
- State: `~/.local/state/terminusbar`
- Data: `~/.local/share/terminusbar`
- Cache and logs: `~/.cache/terminusbar`
- IPC: `$XDG_RUNTIME_DIR/terminusbar-$WAYLAND_DISPLAY.sock`
- Instance lock: `$XDG_RUNTIME_DIR/terminusbar-$WAYLAND_DISPLAY.lock`
- Dmenu IPC: `$XDG_RUNTIME_DIR/terminusbar-dmenu-$WAYLAND_DISPLAY.sock`

XDG base directory variables are respected. `TERMINUSBAR_CONFIG_HOME` overrides the
configuration base (the shell appends `/terminus-est/terminus-bar`); `TERMINUSBAR_STATE_HOME`
and `TERMINUSBAR_DATA_HOME` override their bases (the shell appends `/terminusbar`).
CLI commands, shell completions and plugin script globals use `terminusbar`.
Local plugins remain supported; no upstream plugin repositories are enabled by
default because their script API targets a different shell identity.

Login-screen management belongs to the existing Terminus-Est/Ly integration. This
shell does not install or synchronize a separate display-manager greeter.

## Original top bar

The maintained layout restores the original pre-visual-rework bar: **27px high,
full output width, no top/end margins**, original font/icon scales and accent
colors, with no module boxes. The original `severian@terminus-est ~` launcher,
workspace indicators and app icons with short window titles are on the left.
The bracketed blue clock is centered and now includes the date.

The right side restores the original media title, audio visualizer, tray,
Bluetooth, volume, brightness, RAM/GPU/CPU-cat and network RX/TX indicators,
including their original separators and colors. The weather is retained at the
start of that section, in Celsius, using the configured city rather than IP
geolocation.

`[widget.workspaces] label_source = "kanji"` displays workspace numbers 1–10 as
**一 二 三 四 五 六 七 八 九 十**. Other workspace IDs retain the normal ID
display behavior. This is a native bar-only label choice; it does not rename
compositor workspaces, alter keyboard bindings, or create persistent workspaces.
Existing workspace/output filters and native widget gestures remain in effect.

The Terminus-Bar menu, desktop wallpaper, dashboard, persistent btop and other shell
settings are independent of this bar restoration.

`[widget.media] stacked = true` enables title/artist lines and square artwork on
horizontal bars. `show_controls = true` adds native previous, play/pause and next
buttons, with live playback state and dimmed, inert unsupported actions. Both
options default to false. Controls hide without a player or adequate width, and
compact/vertical/album-art-only layouts remain supported. Playback buttons do
not also open the media panel; the rest of the capsule keeps its usual gestures.

Live settings are `~/.local/state/terminusbar/settings.toml`. Use native settings for
live changes. For direct file edits, stop `terminusbar.service`, preserve the other
settings, validate the merged configuration, then start the service.

## Menu appearance and Terminus-Bar settings

The menu uses a translucent background over the active windows, not a landscape
image or a copy of the desktop wallpaper. Under **Control Center → Effects**,
**Menu background opacity** controls `[control_center] background_opacity`
(default `0.32`), and **Menu transparency** controls `transparency_mode`
(default `"glass"`). Both are independent of the bar and general panel settings.
Text and icons keep their own opacity; glass mode avoids stacking extra dark
fills behind the action rail, Home cards and btop.

The `terminusbar-control-center` layer namespace keeps menu blur separate from the
bar's wallpaper-only compositor rule, for attached and floating menus alike.
The maintained desktop configuration uses `background_opacity = 0.83` (17%
transparent). The local `terminusbar-menu-blur` Hyprland extension supplies size `3`,
two-pass blur with neutral brightness/contrast only during menu rendering.
It restores compositor settings after each draw; the bar and other windows keep
their existing blur. The menu rule's `ignore_alpha = 1.0` remains a no-blur
fallback if the extension is unavailable.
Wide dashboards use one 560-scaled-pixel left controls column, followed by Tasks
and a wider btop. Home combines a small profile header, compact Output/Input and
application volumes, rectangular Wallpaper/Bar and shortcut buttons, and
independent monitor brightness sliders pinned at the bottom. Its player,
clock/weather and decorative quotes are removed; separate media/weather
surfaces and the normal Audio tab remain available.
Below 1500 scaled logical pixels, controls, Tasks, and btop stack in a scrollable
dashboard. Monitor brightness requires a successful hardware brightness query;
a generic `ddcutil detect` "Invalid display" result alone does not rule it out.

Extension source/build: `~/Projects/Terminus-Est/modules/terminus-bar-menu-blur`.
Build with `make -C ~/Projects/Terminus-Est/modules/terminus-bar-menu-blur`; the maintained
Hyprland config registers the artifact on startup and reload. It is pinned to
Hyprland 0.56.2 commit `efb50993780079460b0cbed1363e2166a2de1d9f` and checks the
full API/dependency hash before hooking. After a Hyprland update, review the hook
and rebuild against matching headers/compiler; it refuses an unsupported build
rather than guessing ABI compatibility. Missing artifacts report the rebuild
command. After a failed load and rebuild, an explicit
`hyprctl plugin load ~/Projects/Terminus-Est/modules/terminus-bar-menu-blur/build/terminusbar-menu-blur.so`
may be needed to retry the native loader.

The active dark Sakura Mochi palette is stored in
`~/.config/terminus-est/terminus-bar/palettes/Sakura Mochi.json`. Its primary menu accent,
`#672960`, is persisted as `appearance.accent` by `terminus-settings`.
Change it under **Terminus-Est → Shell accent**, rather than editing the generated
palette primary; ordinary desktop-setting saves then preserve the chosen accent.
The palette's terminal colors are independent of this menu treatment.

In Terminus-Settings, **Terminus-Bar** edits the existing default bar's geometry, widgets
and monitor overrides. This is a display label: the configuration ID remains
`bar.default`, so widget targets and IPC commands are unchanged. The rail's
Settings button opens the same native settings window. Changes still save
through the existing controls; there is no separate Apply/staging workflow.

The **Night Light** rail button and native bar widget use left-click to toggle
the immediate forced filter, and right-click to open the dedicated **Night Light**
settings page. Day/night temperature sliders remain editable with automatic
scheduling disabled. **Open Schedule** leads to the shared Location controls for
sunrise/sunset times or geographic scheduling. Turning off the manual override
returns to the configured schedule; it does not disable an enabled schedule.

The **Terminus-Est** page groups its 36 desktop controls into readable cards for
blur/windows, application and bar backgrounds, window opacity, Terminus-Launcher,
Terminus-Screensaver and Login. Its wallpaper/tint preview and current palette swatches
move below the controls when space is limited. Rows reflow at narrow widths and
increased UI scale without recreating active controls.

Desktop values save on slider release, selection or color confirmation. Login
appearance is saved but staged until **Apply Login** receives administrator
authorization; Ly is not restarted, and authentication settings are unchanged.
Terminus-Screensaver remains a visual idle animation, not a lock screen.

Terminus-Bar claims `org.freedesktop.ScreenSaver` only while at least one native idle
behavior is enabled. With native idle disabled, the external Hypridle daemon
owns application inhibition instead, so VLC playback requests reach the daemon
that actually starts Terminus-Screensaver. Pause/stop release VLC's inhibitor without
changing the configured idle timeout or other applications' inhibitors.

## Synced tasks

The Home dashboard's Tasks panel reads and writes one CalDAV **VTODO collection**,
including lists shared with Tasks.org through Radicale. Configure `[tasks]` with
`enabled`, `collection_url`, `username`, and `sync_interval_seconds` (default 60,
range 15–86400). Use the collection URL, not the server root. See `example.toml`.

Passwords use Secret Service unless `password_file` points to a private,
user-owned credential file. The panel offers masked password entry for Secret
Service accounts. File-backed credentials are edited outside the panel. Normal
TLS certificate checks stay enabled; a private Tailscale HTTPS endpoint works
without disabling them.
Both clients need an active Tailscale connection when using a tailnet-only URL.

Tasks refresh when Terminus-Bar starts, when the Home panel opens, after local changes,
and at the configured interval even while the panel is closed. The panel supports
title, notes, due date, completion/reopening, and confirmed deletion. Clearing a
due date removes it; leaving the displayed date unchanged preserves an existing
due time. Alarms, recurrence, timezone, and provider-specific properties are kept;
configure advanced task behavior in Tasks.org.

The widget has **Active**, **Completed**, **Trash**, and **History** views, searchable
by task title, notes, or activity text. Active tasks can be filtered to Today,
Overdue, Upcoming (next seven days), or No date; task lists support Due and Title
sorting. Counts distinguish the full view from search/filter matches. **Copy to
new task** copies the editor's title, notes, and due date into an unsaved active
task draft; it does not change the source or copy advanced recurrence settings.
The Tasks card uses a local `#f8cd01` accent without changing the global theme.

Confirmed desktop deletions and observed server deletions retain an encrypted
local **Trash** copy. **Undo delete** is offered only when the deletion is safely
unsent. After reconciliation, **Restore copy** creates a fresh synced UID/resource,
preserving the saved calendar metadata and completion state. Pending or conflicting
deletions must be resolved before their saved copies can be permanently removed.
Removing a local copy never deletes a task from the server.

**History** keeps the latest 500 local activity entries, newest first, including
desktop changes and changes observed from the server. History and Trash start
with this update, belong to this desktop and account, and do not sync to other
CalDAV clients. “Discovered” means first observed here, not the task's original
creation time. Saved deletions remain until restored or explicitly removed.
Clearing History requires confirmation, includes entries hidden by search,
and leaves tasks, Trash, and the server unchanged.

The server snapshot, pending changes, Trash, and History are encrypted under
`$TERMINUSBAR_STATE_HOME/terminusbar/tasks/` or the normal XDG state directory. They use
Terminus-Bar's storage master key with a separate task key domain. Unlock/configure
storage before editing; an unsuccessful durable save does not accept a change.
Offline changes survive restarts and replay after reconnection.

Conditional writes prevent silent overwrites. A conflicting local draft remains
visible: edit it to apply only newly changed fields to the latest server version,
or choose **Discard local changes** to keep the server task. A remotely deleted
task is not recreated automatically. Storage recovery refuses known pending task
changes, saved deletions, or history instead of erasing task recovery data.

Sync these tasks through CalDAV, **not Syncthing**. Do not file-sync Radicale's
live collection directory or Terminus-Bar's encrypted task journal between clients.
Tasks.org must finish its own sync before a phone edit can reach the desktop.

## External services and attribution

The shell is derived from [Noctalia](https://github.com/noctalia-dev/noctalia).
Original attribution is preserved in [CREDITS.md](CREDITS.md) and [LICENSE](LICENSE),
as are third-party source licenses.

Existing optional network features still use the real upstream service at
`api.noctalia.dev` for location lookup, public IP lookup, community palettes,
community templates, calendar OAuth and opt-in telemetry. These are external API
identifiers, not Terminus-Bar-owned endpoints. The calendar OAuth request retains its
upstream client identifier. No replacement service URLs are invented.

Contributor-facing architecture and development notes are in
[CONTRIBUTING.md](CONTRIBUTING.md).
