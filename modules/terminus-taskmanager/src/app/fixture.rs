//! A fixed `App` for rendering tests: fixed data, whatever the machine.

use super::*;
use crate::sys::Charge;

/// `n` repeatable pseudo-random samples between `lo` and `hi`.
fn wave(n: usize, seed: u32, lo: f32, hi: f32) -> Hist {
    let mut state = seed;
    let mut hist = Hist::default();
    for _ in 0..n {
        state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        hist.push(lo + (hi - lo) * (state >> 16) as f32 / 65_536.0);
    }
    hist
}

fn process(pid: u32, ppid: u32, name: &str, cmd: &str, cpu: f32, mem_mib: u64, threads: u32) -> Proc {
    let mut hist = [0.0; PROC_HISTORY];
    for (i, slot) in hist.iter_mut().enumerate() {
        *slot = (cpu * (i as f32 + 1.0) / PROC_HISTORY as f32).min(100.0);
    }
    Proc {
        pid,
        ppid,
        name: name.to_string(),
        cmd: cmd.to_string(),
        user: if pid < 100 { "root" } else { "terminus" }.to_string(),
        state: if cpu > 5.0 { State::Running } else { State::Sleeping },
        nice: 0,
        load: Load { cpu, gpu: 0.0, mem: mem_mib << 20, threads },
        vram: 0,
        drm: false,
        gpu_time: 0,
        hist,
        hist_len: PROC_HISTORY,
        start: u64::from(pid) * 100,
        ticks: 0,
        seen: 0,
    }
}

fn disk(name: &str, total_gib: u64, used_gib: u64, with_io: bool) -> Disk {
    let io = if with_io { wave(80, 7, 0.0, 100.0) } else { Hist::default() };
    Disk {
        name: name.to_string(),
        path: format!("/mnt/{name}"),
        block: None,
        prev: None,
        total: total_gib << 30,
        used: used_gib << 30,
        read: 12.5e6,
        write: 480.0e3,
        read_hist: wave(80, 11, 0.0, 30e6),
        write_hist: wave(80, 13, 0.0, 5e6),
        io,
    }
}

impl App {
    /// Sixty-odd processes, eight cores, a GPU, two disks and a battery,
    /// all fixed. Probes the machine once, as `new` does, then overwrites
    /// everything a frame shows.
    pub fn fixture() -> App {
        let mut app = App::new();
        app.interval_ms = 2000;
        app.shown = Shown { cpu: true, mem: true, net: true, proc: true, gpu: true };
        app.paused = false;
        app.dialog = None;
        app.battery =
            Some(Battery { percent: 81.0, charge: Charge::Discharging, watts: Some(7.3), seconds: Some(15_300) });
        app.clock_ticks = 100;
        app.boot = 0;

        app.cpu_name = "Fixture Ryzen 7 5800X 8-Core Processor".to_string();
        app.cpu_pct = 37.0;
        app.cpu_hist = wave(200, 1, 0.0, 100.0);
        app.cores = (0..8).map(|i| Core { pct: 12.0 * i as f32 + 3.0, hist: wave(60, 100 + i, 0.0, 100.0) }).collect();
        app.cpu_temp = Some(61.0);
        app.cpu_temp_crit = 95.0;
        app.cpu_temp_hist = wave(30, 2, 40.0, 80.0);
        app.cpu_mhz = Some(3_850.0);
        app.cpu_watts = Some(42.5);
        app.load = ["0.52".to_string(), "0.61".to_string(), "0.48".to_string()];
        app.uptime = 3 * 86_400 + 4 * 3600 + 12 * 60 + 33;

        let gib = 1u64 << 30;
        app.mem = Mem {
            total: 32 * gib,
            free: 6 * gib,
            available: 19 * gib,
            cached: 9 * gib,
            swap_total: 8 * gib,
            swap_free: 7 * gib,
        };
        app.mem_used = wave(120, 3, 30.0, 45.0);
        app.mem_available = wave(120, 4, 55.0, 70.0);
        app.mem_cached = wave(120, 5, 20.0, 30.0);
        app.mem_free = wave(120, 6, 10.0, 25.0);
        app.disks = vec![disk("root", 931, 402, true), disk("data", 3726, 2900, true), disk("boot", 1, 0, false)];

        app.ifaces = vec!["enp5s0".to_string(), "wlan0".to_string()];
        app.iface = "enp5s0".to_string();
        app.ip = "192.168.1.42".to_string();
        app.rx = 1.5e6;
        app.tx = 82.0e3;
        app.rx_top = 9.0e6;
        app.tx_top = 1.2e6;
        app.rx_total = 48 * gib;
        app.tx_total = 6 * gib;
        app.rx_hist = wave(120, 8, 0.0, 6e6);
        app.tx_hist = wave(120, 9, 0.0, 1e6);

        app.gpu = Some(Gpu::fixture());
        app.gpu_now = GpuSample {
            util: 64.0,
            vram_used: Some(3 * gib),
            vram_total: Some(8 * gib),
            temp: Some(58.0),
            power: Some(96.0),
            core_mhz: Some(1_340),
            mem_mhz: Some(1_750),
            fan_rpm: Some(1_200),
            fan_pct: Some(35.0),
            gtt_used: Some(gib / 2),
            gtt_total: Some(16 * gib),
        };
        app.gpu_hist = wave(120, 21, 0.0, 100.0);
        app.gpu_temp_hist = wave(30, 22, 40.0, 70.0);
        app.vram_hist = wave(120, 23, 30.0, 50.0);

        app.table.clear();
        app.folded.clear();
        let mut procs = vec![
            process(1, 0, "systemd", "/usr/lib/systemd/systemd --system", 0.1, 14, 1),
            process(412, 1, "systemd-journal", "/usr/lib/systemd/systemd-journald", 0.3, 96, 1),
            process(530, 1, "sshd", "sshd: /usr/bin/sshd -D [listener]", 0.0, 8, 1),
            process(900, 1, "Hyprland", "Hyprland", 6.5, 240, 12),
            process(1210, 900, "terminus-bar", "terminus-bar --wallpaper-colours", 2.4, 310, 9),
            process(1300, 900, "kitty", "kitty --class dashboard", 1.1, 120, 4),
            process(1301, 1300, "fish", "fish", 0.0, 22, 1),
            process(1350, 1301, "terminus-taskmanager", "terminus-taskmanager --square", 0.8, 9, 1),
            process(1402, 900, "firefox", "/usr/lib/firefox/firefox -new-window", 18.0, 2_300, 88),
            process(1450, 1402, "Web Content", "/usr/lib/firefox/firefox -contentproc -childID 1", 9.2, 780, 24),
            process(1451, 1402, "Web Content", "/usr/lib/firefox/firefox -contentproc -childID 2", 4.4, 640, 22),
            process(1500, 900, "steam", "/home/terminus/.local/share/Steam/ubuntu12_32/steam", 1.3, 410, 31),
        ];
        for pid in 2000..2048 {
            let parent = if pid % 3 == 0 { 1 } else { 900 };
            let name = format!("worker-{pid}");
            let cmd = format!("/usr/bin/{name} --background --index {}", pid - 2000);
            procs.push(process(
                pid,
                parent,
                &name,
                &cmd,
                (pid % 7) as f32 * 0.4,
                u64::from(pid % 13) * 9 + 4,
                pid % 5 + 1,
            ));
        }
        if let Some(steam_game) = procs.iter_mut().find(|p| p.pid == 1500) {
            steam_game.drm = true;
            steam_game.load.gpu = 42.0;
            steam_game.vram = 1_500 << 20;
        }
        app.gpu_procs = true;
        app.table = procs.into_iter().map(|p| (p.pid, p)).collect();

        app.tree = false;
        app.sort = Sort::Cpu;
        app.reverse = false;
        app.per_core = false;
        app.filter.clear();
        app.filtering = false;
        app.selected = None;
        app.offset = 0;
        app.info = None;
        app.follow = None;
        app.rebuild_view();
        app
    }

    /// The info pane open on `pid`, with fixed details.
    pub fn fixture_info(&mut self, pid: u32) {
        self.info = Some(Info {
            pid,
            cpu: wave(40, 31, 0.0, 60.0),
            what: Some("Standalone web browser from mozilla.org".to_string()),
            package: Some("firefox 160.0.1-1".to_string()),
            path: Some("/usr/lib/firefox/firefox".to_string()),
            unit: Some("app-Hyprland-firefox-1402.scope".to_string()),
            started: "2026-11-22 09:14:03".to_string(),
            cwd: Some("/home/terminus".to_string()),
            cpu_secs: 5_321,
            files: Some(214),
            memory: Some((3_000 << 20, 12 << 20)),
            io: Some((9 << 30, 3 << 30)),
        });
    }
}
