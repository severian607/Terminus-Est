# Mouse

An animated mouse in the bar, the GPU counterpart to the cat. It sleeps when the
GPU is idle, trots as load picks up and sprints under heavy load.

Plugin ID `terminus/mouse`; bar widget `mouse`. A left click opens the gpu view of the
`terminus/vitals` panel (`panel-toggle terminus/vitals:vitals gpu`).
Reads `/sys/class/drm/card{0..3}/device/gpu_busy_percent` (amdgpu only; other
GPUs show a sleeping mouse). Settings mirror the cat plugin
(`mouse_size`, `show_gpu_percent`, `walk_threshold`, `run_threshold`,
`poll_interval`, `mouse_color_mode`, `mouse_color`). Glyph font: `fonts/mousewalk2.otf`.
