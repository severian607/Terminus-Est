//! State and sampling. `tick` takes one sample of everything; rates and
//! percentages need the sample before it, so the first tick only primes them.

use crate::gpu::{Gpu, GpuSample};
use crate::sys::{self, Battery, CpuTimes, DiskIo, Mem, ProcReader, SIGNALS};
use ratatui::{crossterm::event::KeyCode, layout::Rect};
use std::{
    cmp::Ordering,
    collections::{HashMap, HashSet},
    fs,
    path::PathBuf,
    time::Instant,
};

/// Samples kept per graph: two per cell, so enough for 512 columns.
const HISTORY: usize = 1024;
/// Samples behind a process's five-cell graph; they also start the info
/// pane's.
pub const PROC_HISTORY: usize = 10;
/// Deepest tree level drawn; also bounds the walk and fits `Row::guides`.
const MAX_DEPTH: u8 = 60;
/// Rows of the signal list; it fills column by column.
pub const SIGNAL_ROWS: usize = 8;

#[derive(Default)]
pub struct Hist(Vec<f32>);

impl Hist {
    pub fn push(&mut self, v: f32) {
        if self.0.len() == HISTORY {
            self.0.remove(0);
        }
        self.0.push(v);
    }

    pub fn data(&self) -> &[f32] {
        &self.0
    }

    pub fn last(&self) -> f32 {
        self.0.last().copied().unwrap_or(0.0)
    }

    fn clear(&mut self) {
        self.0.clear();
    }
}

#[derive(Default)]
pub struct Core {
    pub pct: f32,
    pub hist: Hist,
}

pub struct Disk {
    pub name: String,
    path: String,
    block: Option<String>,
    prev: Option<DiskIo>,
    pub total: u64,
    pub used: u64,
    /// Bytes per second, now and over time.
    pub read: f64,
    pub write: f64,
    pub read_hist: Hist,
    pub write_hist: Hist,
    /// Percent of the interval the device was busy; empty without I/O stats.
    pub io: Hist,
}

/// What a process, or a folded subtree, is using.
#[derive(Clone, Copy, Default)]
pub struct Load {
    /// Percent of the whole machine; times the core count for per-core.
    pub cpu: f32,
    /// Percent of the GPU's time.
    pub gpu: f32,
    pub mem: u64,
    pub threads: u32,
}

pub struct Proc {
    pub pid: u32,
    pub ppid: u32,
    pub name: String,
    pub cmd: String,
    pub user: String,
    /// The kernel's state letter: R running, S sleeping, T stopped, ...
    pub state: char,
    pub nice: i32,
    pub load: Load,
    /// Bytes of video memory held; 0 without a DRM client.
    pub vram: u64,
    /// Holds a DRM client, so it can use the GPU.
    pub drm: bool,
    gpu_time: u64,
    hist: [f32; PROC_HISTORY],
    hist_len: usize,
    start: u64,
    ticks: u64,
    seen: u64,
}

impl Proc {
    /// Per-core percentages, oldest first.
    pub fn history(&self) -> &[f32] {
        &self.hist[PROC_HISTORY - self.hist_len..]
    }

    fn matches(&self, needle: &str) -> bool {
        let has = |hay: &str| {
            hay.as_bytes().windows(needle.len()).any(|w| w.eq_ignore_ascii_case(needle.as_bytes()))
        };
        needle.is_empty() || has(&self.name) || has(&self.cmd) || has(&self.user) || self.pid.to_string().contains(needle)
    }
}

/// One line of the process list.
pub struct Row {
    pub pid: u32,
    /// Tree level; 0 in the flat list and for roots.
    pub depth: u8,
    /// Bit `n` is set when the ancestor at level `n` has siblings below it,
    /// so its guide line continues through this row.
    pub guides: u64,
    /// Last of its siblings.
    pub last: bool,
    pub kids: bool,
    pub folded: bool,
    /// The process's own load, or its whole subtree's when folded.
    pub load: Load,
}

#[derive(Clone, Copy, PartialEq)]
pub enum Sort {
    Pid,
    Program,
    Command,
    Threads,
    User,
    Memory,
    Gpu,
    Cpu,
}

impl Sort {
    const ALL: [Sort; 8] =
        [Sort::Pid, Sort::Program, Sort::Command, Sort::Threads, Sort::User, Sort::Memory, Sort::Gpu, Sort::Cpu];

    pub fn label(self) -> &'static str {
        match self {
            Sort::Pid => "pid",
            Sort::Program => "program",
            Sort::Command => "command",
            Sort::Threads => "threads",
            Sort::User => "user",
            Sort::Memory => "memory",
            Sort::Cpu => "cpu",
            Sort::Gpu => "gpu",
        }
    }

    pub fn step(self, by: isize) -> Sort {
        let n = Sort::ALL.len() as isize;
        let at = Sort::ALL.iter().position(|s| *s == self).unwrap_or(0) as isize;
        Sort::ALL[(at + by).rem_euclid(n) as usize]
    }

    /// Text columns ascend, numbers put the largest first; pid breaks ties.
    fn order(self, reverse: bool, a: &Proc, b: &Proc, la: &Load, lb: &Load) -> Ordering {
        let order = match self {
            Sort::Pid => Ordering::Equal,
            Sort::Program => a.name.cmp(&b.name),
            Sort::Command => a.cmd.cmp(&b.cmd),
            Sort::User => a.user.cmp(&b.user),
            Sort::Threads => lb.threads.cmp(&la.threads),
            Sort::Memory => lb.mem.cmp(&la.mem),
            Sort::Cpu => lb.cpu.total_cmp(&la.cpu),
            Sort::Gpu => lb.gpu.total_cmp(&la.gpu),
        }
        .then(a.pid.cmp(&b.pid));
        if reverse {
            order.reverse()
        } else {
            order
        }
    }
}

pub struct Shown {
    pub cpu: bool,
    pub mem: bool,
    pub net: bool,
    pub proc: bool,
    pub gpu: bool,
}

pub enum Ask {
    /// Confirm sending this signal.
    Confirm(i32),
    /// Choose from the signal list; the index into `SIGNALS` under the cursor.
    Pick(usize),
}

/// A signal about to be sent to a process.
pub struct Dialog {
    pub pid: u32,
    pub name: String,
    pub ask: Ask,
    pub error: Option<String>,
}

/// What a click does.
#[derive(Clone, Copy)]
pub enum Hit {
    /// The same as pressing this key.
    Key(KeyCode),
    /// Change the update interval by this many milliseconds.
    Interval(i64),
    /// Sort the process list by this column.
    Sort(Sort),
    /// Select this row of the process list.
    Row(usize),
    /// Fold or unfold this row's subtree.
    Fold(usize),
    /// Send this entry of the signal list.
    Signal(usize),
}

/// Click targets of the current frame; later entries lie on top.
pub type Hits = Vec<(Rect, Hit)>;

/// The process shown in the info pane, with what only it is read for.
pub struct Info {
    pub pid: u32,
    /// Per-core CPU percentages, kept for as long as the pane is open.
    pub cpu: Hist,
    /// What the program is, in its package's words.
    pub what: Option<String>,
    /// The package it was installed from, with the version.
    pub package: Option<String>,
    /// The program's file.
    pub path: Option<String>,
    /// The systemd unit it runs in.
    pub unit: Option<String>,
    /// When it started, in local time.
    pub started: String,
    /// The directory it works in. This and the rest change, and are
    /// sampled; all but the CPU time are unreadable for other users'
    /// processes.
    pub cwd: Option<String>,
    /// Seconds of CPU used so far.
    pub cpu_secs: u64,
    pub files: Option<usize>,
    /// Peak resident and swapped-out bytes.
    pub memory: Option<(u64, u64)>,
    /// Bytes read from and written to storage.
    pub io: Option<(u64, u64)>,
}

impl Info {
    fn sample(&mut self, reader: &mut ProcReader, p: &Proc, clock_ticks: u64) {
        self.cwd = reader.cwd(p.pid);
        self.cpu_secs = p.ticks / clock_ticks;
        self.files = reader.files(p.pid);
        self.memory = reader.memory(p.pid);
        self.io = reader.io(p.pid);
    }
}

type Table = HashMap<u32, Proc>;
/// (parent, child), sorted so a parent's children are contiguous. Processes
/// without a listed parent hang off 0.
type Edges = Vec<(u32, u32)>;
/// Per process: its subtree's load, and whether the filter keeps any of it.
type Sums = HashMap<u32, (Load, bool)>;

pub struct App {
    last: Instant,
    pub interval_ms: u64,
    pub shown: Shown,
    pub dialog: Option<Dialog>,
    pub hits: Hits,
    /// Where the process box was drawn, for the scroll wheel.
    pub proc_area: Rect,
    /// Sampling is suspended; the display keeps its last values.
    pub paused: bool,
    battery_dir: Option<PathBuf>,
    pub battery: Option<Battery>,

    cpu_prev: Vec<CpuTimes>,
    cpu_now: Vec<CpuTimes>,
    temp_sensor: Option<PathBuf>,
    freq_files: Vec<PathBuf>,
    energy: Option<(PathBuf, u64)>,
    energy_prev: Option<u64>,
    pub cpu_name: String,
    pub cpu_pct: f32,
    pub cpu_hist: Hist,
    pub cores: Vec<Core>,
    pub cpu_temp: Option<f32>,
    pub cpu_temp_crit: f32,
    pub cpu_temp_hist: Hist,
    pub cpu_mhz: Option<f32>,
    pub cpu_watts: Option<f32>,
    pub load: [String; 3],
    pub uptime: u64,

    pub mem: Mem,
    pub mem_used: Hist,
    pub mem_available: Hist,
    pub mem_cached: Hist,
    pub mem_free: Hist,
    pub disks: Vec<Disk>,

    pub ifaces: Vec<String>,
    pub iface: String,
    net_prev: Option<(u64, u64)>,
    pub ip: String,
    /// Bytes per second.
    pub rx: f32,
    pub tx: f32,
    pub rx_hist: Hist,
    pub tx_hist: Hist,
    pub rx_top: f32,
    pub tx_top: f32,
    pub rx_total: u64,
    pub tx_total: u64,

    pub gpu: Option<Gpu>,
    pub gpu_now: GpuSample,
    pub gpu_hist: Hist,
    pub gpu_temp_hist: Hist,
    pub vram_hist: Hist,

    reader: ProcReader,
    users: HashMap<u32, String>,
    page: u64,
    scan: u64,
    table: Table,
    edges: Edges,
    sums: Sums,
    /// Processes whose subtree is folded in the tree view.
    folded: HashSet<u32>,
    /// The list as shown: filtered, sorted, and nested when `tree` is on.
    pub view: Vec<Row>,
    pub tree: bool,
    pub sort: Sort,
    pub reverse: bool,
    pub per_core: bool,
    pub filter: String,
    pub filtering: bool,
    pub selected: Option<usize>,
    pub offset: usize,
    /// Rows the process list showed last frame, for paging.
    pub page_rows: usize,
    /// The process the info pane shows.
    pub info: Option<Info>,
    /// The process the selection stays on while the list reorders.
    pub follow: Option<u32>,
    /// Whether any process holds a DRM client, so the Gpu% column has data.
    pub gpu_procs: bool,
    clients: Vec<u64>,
    clock_ticks: u64,
    /// Seconds since the epoch at boot.
    boot: u64,
}

fn pct(part: u64, whole: u64) -> f32 {
    if whole == 0 {
        0.0
    } else {
        part as f32 * 100.0 / whole as f32
    }
}

fn children(edges: &[(u32, u32)], parent: u32) -> &[(u32, u32)] {
    let from = edges.partition_point(|e| e.0 < parent);
    let len = edges[from..].partition_point(|e| e.0 == parent);
    &edges[from..from + len]
}

/// Fills `sums` for `pid` and everything below it.
fn total(pid: u32, depth: u8, table: &Table, edges: &[(u32, u32)], filter: &str, sums: &mut Sums) -> (Load, bool) {
    let Some(p) = table.get(&pid) else {
        return (Load::default(), false);
    };
    let (mut load, mut keep) = (p.load, p.matches(filter));
    if depth < MAX_DEPTH {
        for &(_, kid) in children(edges, pid) {
            let (below, kept) = total(kid, depth + 1, table, edges, filter, sums);
            load.cpu += below.cpu;
            load.mem += below.mem;
            load.threads += below.threads;
            load.gpu += below.gpu;
            keep |= kept;
        }
    }
    sums.insert(pid, (load, keep));
    (load, keep)
}

struct Tree<'a> {
    table: &'a Table,
    edges: &'a [(u32, u32)],
    sums: &'a Sums,
    folded: &'a HashSet<u32>,
}

impl Tree<'_> {
    fn kept(&self, pid: u32) -> bool {
        self.sums.get(&pid).is_some_and(|s| s.1)
    }

    /// Appends `pid` and, unless folded, its subtree in sibling order.
    fn emit(&self, pid: u32, depth: u8, guides: u64, last: bool, out: &mut Vec<Row>) {
        let (Some(p), Some(&(sum, true))) = (self.table.get(&pid), self.sums.get(&pid)) else {
            return;
        };
        let kids = children(self.edges, pid);
        let last_kid = kids.iter().rposition(|e| self.kept(e.1));
        let folded = self.folded.contains(&pid);
        let load = if folded { sum } else { p.load };
        out.push(Row { pid, depth, guides, last, kids: last_kid.is_some(), folded, load });
        if folded || depth >= MAX_DEPTH {
            return;
        }
        let guides = if depth > 0 && !last { guides | 1 << depth } else { guides };
        for (i, &(_, kid)) in kids.iter().enumerate() {
            self.emit(kid, depth + 1, guides, Some(i) == last_kid, out);
        }
    }
}

impl App {
    pub fn new() -> App {
        let gpu = Gpu::detect();
        let (temp_sensor, cpu_temp_crit) = sys::cpu_temp_sensor().map_or((None, 95.0), |(p, c)| (Some(p), c));
        let ifaces = sys::interfaces();
        let iface = sys::default_interface()
            .filter(|d| ifaces.contains(d))
            .or_else(|| ifaces.first().cloned())
            .unwrap_or_default();
        let mut app = App {
            last: Instant::now(),
            interval_ms: 2000,
            shown: Shown { cpu: true, mem: true, net: true, proc: true, gpu: gpu.is_some() },
            dialog: None,
            hits: Hits::new(),
            proc_area: Rect::default(),
            cpu_prev: Vec::new(),
            cpu_now: Vec::new(),
            temp_sensor,
            freq_files: sys::cpu_freq_files(),
            energy: sys::cpu_energy_counter(),
            energy_prev: None,
            cpu_name: sys::cpu_name(),
            cpu_pct: 0.0,
            cpu_hist: Hist::default(),
            cores: Vec::new(),
            cpu_temp: None,
            cpu_temp_crit,
            cpu_temp_hist: Hist::default(),
            cpu_mhz: None,
            cpu_watts: None,
            load: Default::default(),
            uptime: 0,
            mem: Mem::default(),
            mem_used: Hist::default(),
            mem_available: Hist::default(),
            mem_cached: Hist::default(),
            mem_free: Hist::default(),
            disks: Vec::new(),
            ifaces,
            iface,
            net_prev: None,
            ip: String::new(),
            rx: 0.0,
            tx: 0.0,
            rx_hist: Hist::default(),
            tx_hist: Hist::default(),
            rx_top: 0.0,
            tx_top: 0.0,
            rx_total: 0,
            tx_total: 0,
            gpu,
            gpu_now: GpuSample::default(),
            gpu_hist: Hist::default(),
            gpu_temp_hist: Hist::default(),
            vram_hist: Hist::default(),
            reader: ProcReader::default(),
            users: sys::users(),
            page: sys::page_size(),
            scan: 0,
            table: Table::new(),
            edges: Edges::new(),
            sums: Sums::new(),
            folded: HashSet::new(),
            clients: Vec::new(),
            clock_ticks: sys::clock_ticks(),
            gpu_procs: false,
            info: None,
            paused: false,
            battery_dir: sys::battery_dir(std::path::Path::new("/sys/class/power_supply")),
            battery: None,
            view: Vec::new(),
            tree: false,
            sort: Sort::Cpu,
            reverse: false,
            per_core: false,
            filter: String::new(),
            filtering: false,
            selected: None,
            offset: 0,
            page_rows: 0,
            follow: None,
            boot: sys::boot_time(),
        };
        app.tick();
        app
    }

    pub fn tick(&mut self) {
        // Paused: keep the last sample on screen. The next one then spans
        // the pause, so its rates are averages over it.
        if self.paused {
            return;
        }
        let now = Instant::now();
        let dt = now.duration_since(self.last).as_secs_f64().max(0.001);
        self.last = now;
        self.battery = self.battery_dir.as_deref().and_then(sys::battery);
        self.tick_proc_gpu(dt);
        let jiffies = self.tick_cpu(dt);
        self.tick_mem();
        self.tick_disks(dt);
        self.tick_net(dt);
        self.tick_gpu();
        self.tick_procs(jiffies);
        self.tick_info();
    }

    pub fn adjust_interval(&mut self, by: i64) {
        self.interval_ms = self.interval_ms.saturating_add_signed(by).clamp(100, 60_000);
    }

    /// Returns the jiffies all cores spent since the last sample: the
    /// denominator for per-process CPU.
    fn tick_cpu(&mut self, dt: f64) -> u64 {
        sys::cpu_times(&mut self.cpu_now);
        let mut jiffies = 0;
        if self.cpu_prev.len() == self.cpu_now.len() && !self.cpu_now.is_empty() {
            let busy = |prev: &CpuTimes, now: &CpuTimes| {
                let total = now.total.saturating_sub(prev.total);
                pct(total.saturating_sub(now.idle.saturating_sub(prev.idle)), total)
            };
            jiffies = self.cpu_now[0].total.saturating_sub(self.cpu_prev[0].total);
            self.cpu_pct = busy(&self.cpu_prev[0], &self.cpu_now[0]);
            self.cpu_hist.push(self.cpu_pct);
            self.cores.resize_with(self.cpu_now.len() - 1, Core::default);
            for (i, core) in self.cores.iter_mut().enumerate() {
                core.pct = busy(&self.cpu_prev[i + 1], &self.cpu_now[i + 1]);
                core.hist.push(core.pct);
            }
        }
        std::mem::swap(&mut self.cpu_prev, &mut self.cpu_now);

        self.cpu_temp = self.temp_sensor.as_ref().and_then(sys::num::<f32>).map(|t| t / 1000.0);
        if let Some(t) = self.cpu_temp {
            self.cpu_temp_hist.push(t);
        }
        let (sum, n) = self
            .freq_files
            .iter()
            .filter_map(sys::num::<f32>)
            .fold((0.0, 0u32), |(sum, n), khz| (sum + khz, n + 1));
        self.cpu_mhz = (n > 0).then(|| sum / n as f32 / 1000.0);
        if let Some((counter, wrap)) = &self.energy {
            if let Some(now) = sys::num::<u64>(counter) {
                if let Some(prev) = self.energy_prev {
                    let joules = if now >= prev { now - prev } else { now + (wrap - prev) } as f64 / 1e6;
                    self.cpu_watts = Some((joules / dt) as f32);
                }
                self.energy_prev = Some(now);
            }
        }
        if let Some(load) = sys::text("/proc/loadavg") {
            for (slot, v) in self.load.iter_mut().zip(load.split(' ')) {
                slot.clear();
                slot.push_str(v);
            }
        }
        self.uptime = fs::read_to_string("/proc/uptime")
            .ok()
            .and_then(|u| u.split(['.', ' ']).next()?.parse().ok())
            .unwrap_or(0);
        jiffies
    }

    fn tick_mem(&mut self) {
        self.mem = sys::mem();
        let m = self.mem;
        self.mem_used.push(pct(m.total.saturating_sub(m.available), m.total));
        self.mem_available.push(pct(m.available, m.total));
        self.mem_cached.push(pct(m.cached, m.total));
        self.mem_free.push(pct(m.free, m.total));
    }

    fn tick_disks(&mut self, dt: f64) {
        let mounts = sys::mounts();
        self.disks.retain(|d| mounts.iter().any(|m| m.path == d.path));
        for m in mounts {
            if self.disks.iter().any(|d| d.path == m.path) {
                continue;
            }
            let name = match m.path.rsplit('/').next() {
                Some(base) if !base.is_empty() => base.to_string(),
                _ => "root".to_string(),
            };
            self.disks.push(Disk {
                name,
                block: sys::block_name(&m.dev),
                path: m.path,
                prev: None,
                total: 0,
                used: 0,
                read: 0.0,
                write: 0.0,
                io: Hist::default(),
                read_hist: Hist::default(),
                write_hist: Hist::default(),
            });
        }
        let stats = sys::diskstats();
        for d in &mut self.disks {
            (d.total, d.used) = sys::usage(&d.path).unwrap_or((0, 0));
            let Some(now) = d.block.as_ref().and_then(|b| stats.get(b)).copied() else {
                continue;
            };
            if let Some(prev) = d.prev {
                let sectors = |a: u64, b: u64| a.saturating_sub(b) as f64 * 512.0 / dt;
                d.read = sectors(now.read_sectors, prev.read_sectors);
                d.write = sectors(now.write_sectors, prev.write_sectors);
                d.read_hist.push(d.read as f32);
                d.write_hist.push(d.write as f32);
                let busy = now.busy_ms.saturating_sub(prev.busy_ms) as f64 / (dt * 10.0);
                d.io.push(busy.min(100.0) as f32);
            }
            d.prev = Some(now);
        }
    }

    fn tick_net(&mut self, dt: f64) {
        if self.iface.is_empty() {
            return;
        }
        self.ip = sys::ipv4(&self.iface).unwrap_or_default();
        let Some((rx, tx)) = sys::net_counters(&self.iface) else {
            return;
        };
        if let Some((prev_rx, prev_tx)) = self.net_prev {
            self.rx = (rx.saturating_sub(prev_rx) as f64 / dt) as f32;
            self.tx = (tx.saturating_sub(prev_tx) as f64 / dt) as f32;
            self.rx_hist.push(self.rx);
            self.tx_hist.push(self.tx);
            self.rx_top = self.rx_top.max(self.rx);
            self.tx_top = self.tx_top.max(self.tx);
        }
        self.net_prev = Some((rx, tx));
        (self.rx_total, self.tx_total) = (rx, tx);
    }

    /// Moves to the next or previous network interface.
    pub fn cycle_iface(&mut self, by: isize) {
        self.ifaces = sys::interfaces();
        if self.ifaces.is_empty() {
            return;
        }
        let at = self.ifaces.iter().position(|i| *i == self.iface).unwrap_or(0) as isize;
        let next = &self.ifaces[(at + by).rem_euclid(self.ifaces.len() as isize) as usize];
        if *next == self.iface {
            return;
        }
        self.iface.clone_from(next);
        self.net_prev = None;
        (self.rx, self.tx, self.rx_top, self.tx_top) = (0.0, 0.0, 0.0, 0.0);
        self.rx_hist.clear();
        self.tx_hist.clear();
        self.tick_net(1.0);
    }

    fn tick_gpu(&mut self) {
        let Some(gpu) = &self.gpu else {
            return;
        };
        self.gpu_now = gpu.sample();
        let g = self.gpu_now;
        self.gpu_hist.push(g.util);
        if let Some(t) = g.temp {
            self.gpu_temp_hist.push(t);
        }
        if let (Some(used), Some(total)) = (g.vram_used, g.vram_total) {
            self.vram_hist.push(pct(used, total));
        }
    }

    /// GPU use per process, from the kernel's DRM accounting. Finding which
    /// processes hold a DRM client means walking every open descriptor, so
    /// that runs on every fifth sample; the others only read known clients.
    fn tick_proc_gpu(&mut self, dt: f64) {
        let everyone = self.scan % 5 == 1;
        for p in self.table.values_mut() {
            if !(p.drm || everyone) {
                continue;
            }
            match self.reader.gpu(p.pid, &mut self.clients) {
                Some((time, vram)) => {
                    if p.drm {
                        // Nanoseconds busy over the interval, as a percentage.
                        p.load.gpu = (time.saturating_sub(p.gpu_time) as f64 / (dt * 1e7)).min(100.0) as f32;
                    }
                    (p.drm, p.gpu_time, p.vram) = (true, time, vram);
                }
                None => (p.drm, p.load.gpu, p.vram) = (false, 0.0, 0),
            }
        }
        self.gpu_procs = self.table.values().any(|p| p.drm);
    }

    /// Feeds the info pane; it closes when its process is gone.
    fn tick_info(&mut self) {
        let Some(mut info) = self.info.take() else {
            return;
        };
        if let Some(p) = self.table.get(&info.pid) {
            info.cpu.push((p.load.cpu * self.cores.len().max(1) as f32).min(100.0));
            info.sample(&mut self.reader, p, self.clock_ticks);
            self.info = Some(info);
        }
    }

    fn tick_procs(&mut self, jiffies: u64) {
        self.scan += 1;
        let cores = self.cores.len().max(1) as f32;
        let Ok(dir) = fs::read_dir("/proc") else {
            return;
        };
        for entry in dir.flatten() {
            let Some(pid) = entry.file_name().to_str().and_then(|n| n.parse::<u32>().ok()) else {
                continue;
            };
            let Some(stat) = self.reader.stat(pid) else {
                continue;
            };
            let (ppid, ticks, start) = (stat.ppid, stat.ticks, stat.start);
            let (state, nice) = (stat.state, stat.nice);
            let (threads, mem) = (stat.threads, stat.rss_pages * self.page);
            // A pid with another start time is a new process.
            if self.table.get(&pid).is_none_or(|p| p.start != start) {
                let name = stat.comm.to_string();
                let cmd = Some(self.reader.cmdline(pid)).filter(|c| !c.is_empty()).unwrap_or_else(|| name.clone());
                // Users outside /etc/passwd (systemd DynamicUser) show as their uid.
                let user = self.reader.uid(pid).map_or_else(String::new, |uid| {
                    self.users.get(&uid).cloned().unwrap_or_else(|| uid.to_string())
                });
                self.table.insert(
                    pid,
                    Proc {
                        pid,
                        ppid,
                        name,
                        cmd,
                        user,
                        state,
                        nice,
                        load: Load { cpu: 0.0, gpu: 0.0, mem, threads },
                        vram: 0,
                        drm: false,
                        gpu_time: 0,
                        hist: [0.0; PROC_HISTORY],
                        hist_len: 0,
                        start,
                        ticks,
                        seen: self.scan,
                    },
                );
                continue;
            }
            let Some(p) = self.table.get_mut(&pid) else {
                continue;
            };
            // The parent changes when a process is adopted.
            (p.ppid, p.state, p.nice, p.seen) = (ppid, state, nice, self.scan);
            (p.load.threads, p.load.mem) = (threads, mem);
            if jiffies > 0 {
                p.load.cpu = pct(ticks.saturating_sub(p.ticks), jiffies);
                p.hist.rotate_left(1);
                p.hist[PROC_HISTORY - 1] = (p.load.cpu * cores).min(100.0);
                p.hist_len = (p.hist_len + 1).min(PROC_HISTORY);
            }
            p.ticks = ticks;
        }
        let scan = self.scan;
        self.table.retain(|_, p| p.seen == scan);
        let table = &self.table;
        self.folded.retain(|pid| table.contains_key(pid));
        self.rebuild_view();
    }

    pub fn proc(&self, pid: u32) -> Option<&Proc> {
        self.table.get(&pid)
    }

    /// Re-filters and re-sorts the list. The selection keeps its row, or
    /// its process while following.
    pub fn rebuild_view(&mut self) {
        self.view.clear();
        let (table, sort, reverse) = (&self.table, self.sort, self.reverse);
        if self.tree {
            let (edges, sums) = (&mut self.edges, &mut self.sums);
            edges.clear();
            edges.extend(table.values().map(|p| (if table.contains_key(&p.ppid) { p.ppid } else { 0 }, p.pid)));
            edges.sort_unstable();
            sums.clear();
            for i in 0..children(edges, 0).len() {
                total(edges[i].1, 0, table, edges, &self.filter, sums);
            }
            // Siblings in the list's order, by what their subtrees use.
            edges.sort_unstable_by(|a, b| {
                a.0.cmp(&b.0).then_with(|| sort.order(reverse, &table[&a.1], &table[&b.1], &sums[&a.1].0, &sums[&b.1].0))
            });
            let tree = Tree { table, edges, sums, folded: &self.folded };
            for &(_, root) in children(edges, 0) {
                tree.emit(root, 0, 0, false, &mut self.view);
            }
        } else {
            let rows = table.values().filter(|p| p.matches(&self.filter));
            self.view.extend(rows.map(|p| Row {
                pid: p.pid,
                depth: 0,
                guides: 0,
                last: false,
                kids: false,
                folded: false,
                load: p.load,
            }));
            self.view.sort_unstable_by(|a, b| sort.order(reverse, &table[&a.pid], &table[&b.pid], &a.load, &b.load));
        }
        // Following ends when its process leaves the list.
        if let Some(pid) = self.follow {
            match self.view.iter().position(|row| row.pid == pid) {
                Some(row) => self.selected = Some(row),
                None => self.follow = None,
            }
        }
        self.selected = match self.selected {
            Some(_) if self.view.is_empty() => None,
            Some(row) => Some(row.min(self.view.len() - 1)),
            None => None,
        };
    }

    fn selected_pid(&self) -> Option<u32> {
        self.selected.and_then(|row| self.view.get(row)).map(|row| row.pid)
    }

    /// Starts following the selected process, or stops following.
    pub fn toggle_follow(&mut self) {
        self.follow = match self.follow {
            Some(_) => None,
            None => self.selected_pid(),
        };
    }

    /// Call after the selection was moved by hand: following goes with it.
    pub fn refollow(&mut self) {
        if self.follow.is_some() {
            self.follow = self.selected_pid();
        }
    }

    /// Moves the selection; stepping up from the first row clears it.
    pub fn select(&mut self, by: isize) {
        if self.view.is_empty() {
            return;
        }
        self.selected = match self.selected {
            None if by > 0 => Some(0),
            None => None,
            Some(0) if by < 0 => None,
            Some(row) => Some(row.saturating_add_signed(by).min(self.view.len() - 1)),
        };
    }

    pub fn set_sort(&mut self, sort: Sort) {
        self.sort = sort;
        self.rebuild_view();
    }

    /// Opens the info pane on the selected process; closes it when that
    /// process is already shown or nothing is selected.
    pub fn toggle_info(&mut self) {
        let target = self.selected_pid();
        let shown = self.info.as_ref().map(|info| info.pid);
        self.info = target.filter(|pid| shown != Some(*pid)).and_then(|pid| {
            let p = self.table.get(&pid)?;
            let mut cpu = Hist::default();
            p.history().iter().for_each(|v| cpu.push(*v));
            // Other users' processes hide the link to their file; the
            // command line often names it.
            let argv0 = || p.cmd.split(' ').next().filter(|arg| arg.starts_with('/')).map(String::from);
            let path = self.reader.exe(pid).or_else(argv0);
            let (package, what) = if pid == 2 || p.ppid == 2 {
                (None, Some("Kernel thread".to_string()))
            } else {
                path.as_deref().and_then(sys::package).map_or((None, None), |(package, what)| (Some(package), what))
            };
            let mut info = Info {
                pid,
                cpu,
                what,
                package,
                path,
                unit: self.reader.unit(pid),
                started: sys::local_time(self.boot + p.start / self.clock_ticks),
                cwd: None,
                cpu_secs: 0,
                files: None,
                memory: None,
                io: None,
            };
            info.sample(&mut self.reader, p, self.clock_ticks);
            Some(info)
        });
    }

    /// Seconds since the process started.
    pub fn elapsed(&self, p: &Proc) -> u64 {
        self.uptime.saturating_sub(p.start / self.clock_ticks)
    }

    /// The wheel: moves the selection, or the list when nothing is selected.
    pub fn scroll(&mut self, by: isize) {
        match self.selected {
            Some(row) => self.selected = Some(row.saturating_add_signed(by).min(self.view.len().saturating_sub(1))),
            None => self.offset = self.offset.saturating_add_signed(by),
        }
    }

    /// Folds (`Some(true)`), unfolds or toggles the subtree under `row`.
    pub fn fold(&mut self, row: usize, to: Option<bool>) {
        let Some(r) = self.view.get(row).filter(|r| self.tree && r.kids) else {
            return;
        };
        let pid = r.pid;
        if to.unwrap_or(!r.folded) {
            self.folded.insert(pid);
        } else {
            self.folded.remove(&pid);
        }
        self.rebuild_view();
    }

    fn open(&mut self, ask: Ask) {
        let target = self.selected.and_then(|row| self.view.get(row)).and_then(|row| self.table.get(&row.pid));
        if let Some(p) = target {
            self.dialog = Some(Dialog { pid: p.pid, name: p.name.clone(), ask, error: None });
        }
    }

    /// Asks before sending `signal` to the selected process.
    pub fn ask(&mut self, signal: i32) {
        self.open(Ask::Confirm(signal));
    }

    /// Opens the signal list for the selected process, on SIGTERM.
    pub fn pick(&mut self) {
        self.open(Ask::Pick(SIGNALS.iter().position(|s| s.0 == libc::SIGTERM).unwrap_or(0)));
    }

    /// Moves the signal list's cursor, staying put at the edges.
    pub fn pick_move(&mut self, by: isize) {
        if let Some(Dialog { ask: Ask::Pick(cursor), .. }) = &mut self.dialog {
            if let Some(next) = cursor.checked_add_signed(by).filter(|i| *i < SIGNALS.len()) {
                *cursor = next;
            }
        }
    }

    /// Sends entry `index` of the signal list.
    pub fn pick_send(&mut self, index: usize) {
        if let Some(Dialog { ask: Ask::Pick(cursor), .. }) = &mut self.dialog {
            *cursor = index.min(SIGNALS.len() - 1);
            self.confirm();
        }
    }

    /// Sends the dialog's signal; a failure stays in the dialog.
    pub fn confirm(&mut self) {
        let Some(dialog) = &mut self.dialog else {
            return;
        };
        let signal = match dialog.ask {
            Ask::Confirm(signal) => signal,
            Ask::Pick(cursor) => SIGNALS[cursor].0,
        };
        match sys::kill(dialog.pid, signal) {
            Ok(()) => self.dialog = None,
            Err(e) => {
                let text = e.to_string();
                // "Operation not permitted (os error 1)": the reason is enough.
                dialog.error = Some(text.split(" (os error").next().unwrap_or(&text).to_string());
            }
        }
    }
}
