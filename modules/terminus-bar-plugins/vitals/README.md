# Vitals

The click panel of the bar's `cat` (cpu), `mouse` (gpu) and `mem` widgets: one bar-attached
panel, three views, in the terminal look of the built-in quick panels.

Plugin ID `terminus/vitals`; panel `vitals`.

```sh
terminus-bar msg panel-toggle terminus/vitals:vitals cpu   # or gpu, mem
```

| View | Hero | ²top |
| --- | --- | --- |
| cpu | the cat, usage, model · temp · clock; header = CPU pressure | processes by average share of the machine over the window |
| gpu | the mouse, usage, name · temp · clock · fan; header = VRAM used / total | processes by peak resident VRAM + GTT (DRM fdinfo) |
| mem | used GiB, percent, available of total; header = total | processes by peak memory (PSS) |

- `¹history` is a band graph from the logger (`terminus-vitals-log.service`, `bin/vitals-log`),
  which logs one row per minute: faint min–max band, bright mark at the average (gpu: solid bar at the
  average, mark at the 95th percentile). Event marks sit underneath: `▲` hot, `✖` oom, `↻` reboot.
  `h` cycles the window: 1, 6, 24 hours, and 7 days once a day has been logged. `bin/vitals-query` reads the log.
- The hero text turns warning or error by the same rules as the bar's health checks (`system/health.cpp`).
- Keys: `Tab` / `Shift+Tab` cycle cpu → gpu → mem in place, `h` cycles the history window, `t` opens
  `terminus-taskmanager` in Ghostty, `Esc` closes.
- The cat and mouse declare the click in their manifests (`[widget.actions]`); the `mem` sysmon
  widget's click is set in `config/terminus-bar/97-vitals.toml`.
- Animal fonts are loaded from the sibling `cat` and `mouse` plugin folders.
