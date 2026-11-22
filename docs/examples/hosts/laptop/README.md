# Example host: a laptop

Copy this directory to `config/hosts/<hostname>/` (the name in `/etc/hostname`) and edit it. Every file is optional; delete the ones you do not need. `docs/ARCHITECTURE.md` lists what each one does.

| File | What it does here |
|---|---|
| `hyprland/monitors.lua` | Names the built-in panel and an external output, which mirrors the panel |
| `hyprland/host.lua` | Touchpad behaviour and a per-device pointer speed |
| `hyprland/host_bindings.lua` | Super+L becomes the authenticating lock |
| `hypridle.conf` | Screensaver after 7 minutes, lock after 10, suspend after 30 |
| `secure-lock` | Makes `terminus system lock` authenticate on this machine |
| `screensaver-art` | Picks the alternative screensaver artwork |

The authenticating lock (`terminus-lock`) needs a hyprlock build that reports readiness with `--ready-fd`; point `TERMINUS_HYPRLOCK` at it in `~/.config/uwsm/env`. Without one, leave out `secure-lock`, `hypridle.conf` and the Super+L binding: the screensaver alone does not lock.

Find your names with `hyprctl monitors` and `hyprctl devices`.
