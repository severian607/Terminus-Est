# Terminus-TaskManager

A system monitor for the terminal in btop's design, written in Rust (ratatui). GPU and CPU sit on top; memory, disks and network on the left; processes on the right. `bin/terminus-taskmanager` builds it with cargo on first run (`target/` is not tracked).

## Keys

The highlighted letter in each box title is its key. Titles, column headings, rows and the signal list also take clicks.

| Key | Action |
|---|---|
| `1`-`5` | Show or hide a box |
| `f` | Filter |
| `c` / `m` / `g` | Sort by CPU / memory / GPU |
| `←` / `→` | Sort by any column |
| `r` | Reverse the sort |
| `o` | Per-core CPU |
| `e` | Tree view; `space`, `+`, `-` fold |
| `↑` / `↓`, wheel | Select a process |
| `enter` | Info pane (asks pacman what the program is) |
| `F` | Follow the selected process |
| `t` / `k` | Terminate / kill |
| `s` | Signal list |
| `p` | Pause |
| `b` / `n` | Previous / next network interface |
| `+` / `-` | Update interval |
| `q` | Quit |

## Data and colours

- Colours come from the btop theme Terminus-Bar renders (`~/.config/btop/themes/terminusbar.theme`), else from `config/settings.toml`. Loads turn amber from 75% and red from 90%.
- System data comes from `/proc` and `/sys`. The AMD GPU is read from sysfs (no ROCm), per-process GPU use from DRM fdinfo.
- CPU watts need `config/udev/70-terminus-cpu-energy.rules` copied to `/etc/udev/rules.d/` (copied, not linked: udev may run before `/home` is mounted).
