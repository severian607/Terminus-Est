//! Readers for /proc and /sys. Linux only.

use std::{
    collections::HashMap,
    ffi::{CStr, CString},
    fmt::Write as _,
    fs,
    io::{self, Read},
    net::Ipv4Addr,
    os::unix::fs::MetadataExt,
    path::{Path, PathBuf},
    process::Command,
    str::FromStr,
};

pub fn text(path: impl AsRef<Path>) -> Option<String> {
    let mut s = fs::read_to_string(path).ok()?;
    s.truncate(s.trim_end().len());
    Some(s)
}

pub fn num<T: FromStr>(path: impl AsRef<Path>) -> Option<T> {
    fs::read_to_string(path).ok()?.trim().parse().ok()
}

// CPU

#[derive(Clone, Copy, Default)]
pub struct CpuTimes {
    pub total: u64,
    pub idle: u64,
}

/// Jiffies from /proc/stat: the aggregate first, then one entry per core.
pub fn cpu_times(out: &mut Vec<CpuTimes>) {
    out.clear();
    let Ok(stat) = fs::read_to_string("/proc/stat") else {
        return;
    };
    for line in stat.lines() {
        // The cpu lines lead the file.
        let Some(rest) = line.strip_prefix("cpu") else {
            break;
        };
        let mut fields = rest.split_ascii_whitespace();
        if !rest.starts_with(' ') {
            fields.next(); // core number
        }
        let mut t = CpuTimes::default();
        // user nice system idle iowait irq softirq steal
        for (i, v) in fields.take(8).enumerate() {
            let v: u64 = v.parse().unwrap_or(0);
            t.total += v;
            if i == 3 || i == 4 {
                t.idle += v;
            }
        }
        out.push(t);
    }
}

/// "AMD Ryzen 5 5600X 6-Core Processor" -> "Ryzen 5 5600X".
pub fn cpu_name() -> String {
    let info = fs::read_to_string("/proc/cpuinfo").unwrap_or_default();
    let model = info
        .lines()
        .find_map(|l| l.strip_prefix("model name"))
        .and_then(|l| l.split_once(':'))
        .map_or("CPU", |(_, v)| v.trim());
    let model = model.split(" @").next().unwrap_or(model);
    let noise = |w: &str| {
        matches!(w, "AMD" | "Intel(R)" | "Core(TM)" | "CPU" | "Processor" | "Gen")
            || w.ends_with("-Core")
            || (w.ends_with("th") && w[..w.len() - 2].bytes().all(|b| b.is_ascii_digit()))
    };
    let name: Vec<&str> = model.split_whitespace().filter(|w| !noise(w)).collect();
    if name.is_empty() {
        model.to_string()
    } else {
        name.join(" ")
    }
}

/// Package temperature sensor and its critical point in °C.
pub fn cpu_temp_sensor() -> Option<(PathBuf, f32)> {
    fs::read_dir("/sys/class/hwmon").ok()?.flatten().find_map(|e| {
        let dir = e.path();
        let name = text(dir.join("name"))?;
        if !matches!(name.as_str(), "k10temp" | "coretemp" | "zenpower" | "cpu_thermal") {
            return None;
        }
        let input = dir.join("temp1_input");
        input.exists().then(|| {
            let crit = num::<f32>(dir.join("temp1_crit")).map_or(95.0, |c| c / 1000.0);
            (input, crit)
        })
    })
}

pub fn cpu_freq_files() -> Vec<PathBuf> {
    let Ok(dir) = fs::read_dir("/sys/devices/system/cpu") else {
        return Vec::new();
    };
    dir.flatten()
        .map(|e| e.path().join("cpufreq/scaling_cur_freq"))
        .filter(|p| p.exists())
        .collect()
}

/// The CPU package's energy counter (microjoules) and the value it wraps
/// at. The kernel keeps it root-only unless the machine grants access.
pub fn cpu_energy_counter() -> Option<(PathBuf, u64)> {
    let zone = Path::new("/sys/class/powercap/intel-rapl:0");
    let counter = zone.join("energy_uj");
    num::<u64>(&counter)?;
    Some((counter, num(zone.join("max_energy_range_uj")).unwrap_or(u64::MAX)))
}

// Memory

#[derive(Clone, Copy, Default)]
pub struct Mem {
    pub total: u64,
    pub free: u64,
    pub available: u64,
    pub cached: u64,
    pub swap_total: u64,
    pub swap_free: u64,
}

pub fn mem() -> Mem {
    let mut m = Mem::default();
    let Ok(info) = fs::read_to_string("/proc/meminfo") else {
        return m;
    };
    for line in info.lines() {
        let Some((key, rest)) = line.split_once(':') else {
            continue;
        };
        let slot = match key {
            "MemTotal" => &mut m.total,
            "MemFree" => &mut m.free,
            "MemAvailable" => &mut m.available,
            "Cached" => &mut m.cached,
            "SwapTotal" => &mut m.swap_total,
            "SwapFree" => &mut m.swap_free,
            _ => continue,
        };
        let kib: u64 = rest.split_ascii_whitespace().next().and_then(|v| v.parse().ok()).unwrap_or(0);
        *slot = kib * 1024;
    }
    m
}

// Disks

pub struct Mount {
    pub dev: String,
    pub path: String,
}

/// /proc/mounts escapes space, tab, newline and backslash as octal.
fn unescape(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        let octal = (bytes[i] == b'\\')
            .then(|| bytes.get(i + 1..i + 4))
            .flatten()
            .and_then(|digits| u8::from_str_radix(std::str::from_utf8(digits).ok()?, 8).ok());
        match octal {
            Some(b) => {
                out.push(b);
                i += 4;
            }
            None => {
                out.push(bytes[i]);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Mounted block devices, one entry per device (its first mount, so btrfs
/// subvolumes and bind mounts do not repeat the same filesystem).
pub fn mounts() -> Vec<Mount> {
    let mut out: Vec<Mount> = Vec::new();
    let Ok(table) = fs::read_to_string("/proc/self/mounts") else {
        return out;
    };
    for line in table.lines() {
        let mut f = line.split(' ');
        let (Some(dev), Some(path)) = (f.next(), f.next()) else {
            continue;
        };
        if !dev.starts_with("/dev/") || dev.starts_with("/dev/loop") || out.iter().any(|m| m.dev == dev) {
            continue;
        }
        out.push(Mount { dev: dev.to_string(), path: unescape(path) });
    }
    out
}

/// (total, used) bytes of the filesystem at `path`, used = total minus what unprivileged users may still allocate, as btop counts it.
pub fn usage(path: &str) -> Option<(u64, u64)> {
    let c = CString::new(path).ok()?;
    // SAFETY: `c` is a valid NUL-terminated path and `vfs` is a plain C struct
    // that statvfs fills in; it is read only after the call reports success.
    let vfs = unsafe {
        let mut vfs: libc::statvfs = std::mem::zeroed();
        if libc::statvfs(c.as_ptr(), &mut vfs) != 0 {
            return None;
        }
        vfs
    };
    let block = vfs.f_frsize as u64;
    let total = vfs.f_blocks as u64 * block;
    Some((total, total.saturating_sub(vfs.f_bavail as u64 * block)))
}

/// Kernel name of a block device node ("/dev/mapper/root" -> "dm-0").
pub fn block_name(dev: &str) -> Option<String> {
    Some(fs::canonicalize(dev).ok()?.file_name()?.to_string_lossy().into_owned())
}

#[derive(Clone, Copy)]
pub struct DiskIo {
    pub read_sectors: u64,
    pub write_sectors: u64,
    pub busy_ms: u64,
}

pub fn diskstats() -> HashMap<String, DiskIo> {
    let stats = fs::read_to_string("/proc/diskstats").unwrap_or_default();
    stats
        .lines()
        .filter_map(|line| {
            let f: Vec<&str> = line.split_ascii_whitespace().collect();
            let field = |i: usize| f.get(i)?.parse::<u64>().ok();
            Some((
                f.get(2)?.to_string(),
                DiskIo { read_sectors: field(5)?, write_sectors: field(9)?, busy_ms: field(12)? },
            ))
        })
        .collect()
}

// Network

pub fn interfaces() -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir("/sys/class/net")
        .map(|d| d.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default();
    names.retain(|n| n != "lo");
    names.sort();
    names
}

/// The interface carrying the default route.
pub fn default_interface() -> Option<String> {
    let routes = fs::read_to_string("/proc/net/route").ok()?;
    routes.lines().skip(1).find_map(|line| {
        let mut f = line.split_ascii_whitespace();
        let (iface, destination) = (f.next()?, f.next()?);
        (destination == "00000000").then(|| iface.to_string())
    })
}

/// (received, transmitted) byte counters.
pub fn net_counters(iface: &str) -> Option<(u64, u64)> {
    let dir = Path::new("/sys/class/net").join(iface).join("statistics");
    Some((num(dir.join("rx_bytes"))?, num(dir.join("tx_bytes"))?))
}

pub fn ipv4(iface: &str) -> Option<String> {
    let mut list: *mut libc::ifaddrs = std::ptr::null_mut();
    // SAFETY: getifaddrs hands back a linked list it owns; every node and the
    // name and address it points to stay valid until freeifaddrs below, and
    // an AF_INET address is a sockaddr_in.
    unsafe {
        if libc::getifaddrs(&mut list) != 0 {
            return None;
        }
        let mut found = None;
        let mut node = list;
        while !node.is_null() {
            let entry = &*node;
            if !entry.ifa_addr.is_null()
                && i32::from((*entry.ifa_addr).sa_family) == libc::AF_INET
                && CStr::from_ptr(entry.ifa_name).to_bytes() == iface.as_bytes()
            {
                let addr = &*(entry.ifa_addr as *const libc::sockaddr_in);
                found = Some(Ipv4Addr::from(u32::from_be(addr.sin_addr.s_addr)).to_string());
                break;
            }
            node = entry.ifa_next;
        }
        libc::freeifaddrs(list);
        found
    }
}

// Processes

pub fn page_size() -> u64 {
    // SAFETY: sysconf takes an integer and returns one.
    let size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if size > 0 {
        size as u64
    } else {
        4096
    }
}

pub fn users() -> HashMap<u32, String> {
    let passwd = fs::read_to_string("/etc/passwd").unwrap_or_default();
    passwd
        .lines()
        .filter_map(|line| {
            let mut f = line.split(':');
            let name = f.next()?;
            Some((f.nth(1)?.parse().ok()?, name.to_string()))
        })
        .collect()
}

pub fn kill(pid: u32, signal: i32) -> io::Result<()> {
    // SAFETY: kill takes plain integers and touches no memory of ours.
    if unsafe { libc::kill(pid as libc::pid_t, signal) } == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// Jiffies per second, the unit of a process's start time.
pub fn clock_ticks() -> u64 {
    // SAFETY: sysconf takes an integer and returns one.
    let ticks = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if ticks > 0 {
        ticks as u64
    } else {
        100
    }
}

/// Seconds since the epoch at boot, to date a process's start.
pub fn boot_time() -> u64 {
    let stat = fs::read_to_string("/proc/stat").unwrap_or_default();
    stat.lines().find_map(|l| l.strip_prefix("btime ")?.trim().parse().ok()).unwrap_or(0)
}

/// `Wed 18 Nov 13:02`, in local time.
pub fn local_time(epoch: u64) -> String {
    let (time, mut out) = (epoch as libc::time_t, [0u8; 32]);
    // SAFETY: localtime_r fills the tm it is handed, and strftime writes no
    // more than the length it is given into `out`.
    let len = unsafe {
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&time, &mut tm);
        libc::strftime(out.as_mut_ptr().cast(), out.len(), c"%a %d %b %H:%M".as_ptr(), &tm)
    };
    String::from_utf8_lossy(&out[..len]).into_owned()
}

/// The pacman package owning the file at `path`: its name and version,
/// and its description when the local database has one.
pub fn package(path: &str) -> Option<(String, Option<String>)> {
    // A program replaced by an update still runs from the old file.
    let path = path.strip_suffix(" (deleted)").unwrap_or(path);
    let out = Command::new("pacman").args(["-Qo", path]).env("LC_ALL", "C").output().ok()?;
    // "/usr/lib/firefox/firefox is owned by firefox 160.0.1-1"
    let line = String::from_utf8_lossy(&out.stdout);
    let (name, version) = line.trim().rsplit_once(" is owned by ")?.1.split_once(' ')?;
    let what = fs::read_to_string(format!("/var/lib/pacman/local/{name}-{version}/desc"))
        .ok()
        .and_then(|desc| desc.lines().skip_while(|l| *l != "%DESC%").nth(1).map(String::from));
    Some((format!("{name} {version}"), what))
}

/// The standard signals in number order, for the signal list.
pub const SIGNALS: [(i32, &str); 31] = [
    (libc::SIGHUP, "SIGHUP"),
    (libc::SIGINT, "SIGINT"),
    (libc::SIGQUIT, "SIGQUIT"),
    (libc::SIGILL, "SIGILL"),
    (libc::SIGTRAP, "SIGTRAP"),
    (libc::SIGABRT, "SIGABRT"),
    (libc::SIGBUS, "SIGBUS"),
    (libc::SIGFPE, "SIGFPE"),
    (libc::SIGKILL, "SIGKILL"),
    (libc::SIGUSR1, "SIGUSR1"),
    (libc::SIGSEGV, "SIGSEGV"),
    (libc::SIGUSR2, "SIGUSR2"),
    (libc::SIGPIPE, "SIGPIPE"),
    (libc::SIGALRM, "SIGALRM"),
    (libc::SIGTERM, "SIGTERM"),
    (libc::SIGSTKFLT, "SIGSTKFLT"),
    (libc::SIGCHLD, "SIGCHLD"),
    (libc::SIGCONT, "SIGCONT"),
    (libc::SIGSTOP, "SIGSTOP"),
    (libc::SIGTSTP, "SIGTSTP"),
    (libc::SIGTTIN, "SIGTTIN"),
    (libc::SIGTTOU, "SIGTTOU"),
    (libc::SIGURG, "SIGURG"),
    (libc::SIGXCPU, "SIGXCPU"),
    (libc::SIGXFSZ, "SIGXFSZ"),
    (libc::SIGVTALRM, "SIGVTALRM"),
    (libc::SIGPROF, "SIGPROF"),
    (libc::SIGWINCH, "SIGWINCH"),
    (libc::SIGIO, "SIGIO"),
    (libc::SIGPWR, "SIGPWR"),
    (libc::SIGSYS, "SIGSYS"),
];

pub struct Stat<'a> {
    pub comm: &'a str,
    /// R running, S sleeping, D disk wait, Z zombie, T stopped, I idle.
    pub state: char,
    pub ppid: u32,
    pub nice: i32,
    /// utime + stime, in the jiffies /proc/stat counts.
    pub ticks: u64,
    pub threads: u32,
    pub start: u64,
    pub rss_pages: u64,
}

/// Reads per-process files through buffers reused across the whole scan.
#[derive(Default)]
pub struct ProcReader {
    path: String,
    raw: Vec<u8>,
    text: String,
}

impl ProcReader {
    /// Reads the file at `self.path` into `self.raw`.
    fn read(&mut self) -> bool {
        self.raw.clear();
        fs::File::open(&self.path).and_then(|mut f| f.read_to_end(&mut self.raw)).is_ok()
    }

    fn load(&mut self, pid: u32, file: &str) -> bool {
        self.path.clear();
        let _ = write!(self.path, "/proc/{pid}/{file}");
        self.read()
    }

    pub fn stat(&mut self, pid: u32) -> Option<Stat<'_>> {
        if !self.load(pid, "stat") {
            return None;
        }
        self.text.clear();
        self.text.push_str(&String::from_utf8_lossy(&self.raw));
        let s = self.text.as_str();
        // comm may hold spaces and parentheses; the last ')' ends it.
        let (open, close) = (s.find('(')?, s.rfind(')')?);
        // Fields after comm start at field 3 (state), so field N is index N-3.
        let mut f = s[close + 1..].split_ascii_whitespace();
        let state = f.next()?.chars().next()?; // 3
        let ppid = f.next()?.parse().ok()?; // 4
        let utime: u64 = f.nth(9)?.parse().ok()?; // 14
        let stime: u64 = f.next()?.parse().ok()?; // 15
        let nice = f.nth(3)?.parse().ok()?; // 19
        let threads = f.next()?.parse().ok()?; // 20
        let start = f.nth(1)?.parse().ok()?; // 22
        let rss_pages = f.nth(1)?.parse().ok()?; // 24
        Some(Stat { comm: &s[open + 1..close], state, ppid, nice, ticks: utime + stime, threads, start, rss_pages })
    }

    pub fn cmdline(&mut self, pid: u32) -> String {
        if !self.load(pid, "cmdline") {
            return String::new();
        }
        self.raw.truncate(1024);
        while self.raw.last() == Some(&0) {
            self.raw.pop();
        }
        for b in &mut self.raw {
            if *b == 0 || *b == b'\n' {
                *b = b' ';
            }
        }
        String::from_utf8_lossy(&self.raw).into_owned()
    }

    pub fn uid(&mut self, pid: u32) -> Option<u32> {
        self.path.clear();
        let _ = write!(self.path, "/proc/{pid}");
        fs::metadata(&self.path).ok().map(|m| m.uid())
    }

    /// Bytes the process has read from and written to storage. Other
    /// users' processes do not show theirs.
    pub fn io(&mut self, pid: u32) -> Option<(u64, u64)> {
        if !self.load(pid, "io") {
            return None;
        }
        let text = String::from_utf8_lossy(&self.raw);
        let field = |key: &str| text.lines().find_map(|l| l.strip_prefix(key)?.trim().parse::<u64>().ok());
        Some((field("read_bytes:")?, field("write_bytes:")?))
    }

    fn link(&mut self, pid: u32, file: &str) -> Option<String> {
        self.path.clear();
        let _ = write!(self.path, "/proc/{pid}/{file}");
        fs::read_link(&self.path).ok().map(|target| target.to_string_lossy().into_owned())
    }

    /// The program's file. Kernel threads have none, and other users'
    /// processes hide theirs, as they do the next three.
    pub fn exe(&mut self, pid: u32) -> Option<String> {
        self.link(pid, "exe")
    }

    /// The directory the process works in.
    pub fn cwd(&mut self, pid: u32) -> Option<String> {
        self.link(pid, "cwd")
    }

    /// How many files the process holds open.
    pub fn files(&mut self, pid: u32) -> Option<usize> {
        self.path.clear();
        let _ = write!(self.path, "/proc/{pid}/fd");
        fs::read_dir(&self.path).ok().map(|dir| dir.count())
    }

    /// The most memory the process has held, and what of it is swapped
    /// out, in bytes.
    pub fn memory(&mut self, pid: u32) -> Option<(u64, u64)> {
        if !self.load(pid, "status") {
            return None;
        }
        let text = String::from_utf8_lossy(&self.raw);
        let field = |key: &str| {
            let kib = text.lines().find_map(|l| l.strip_prefix(key)?.trim().strip_suffix("kB"))?;
            kib.trim().parse::<u64>().ok().map(|kib| kib * 1024)
        };
        Some((field("VmHWM:")?, field("VmSwap:")?))
    }

    /// The systemd unit the process runs in: the last step of its cgroup.
    pub fn unit(&mut self, pid: u32) -> Option<String> {
        if !self.load(pid, "cgroup") {
            return None;
        }
        let text = String::from_utf8_lossy(&self.raw);
        let unit = text.lines().find_map(|l| l.strip_prefix("0::"))?.rsplit('/').next()?;
        (!unit.is_empty()).then(|| unit.to_string())
    }

    /// GPU time in nanoseconds (all engines) and VRAM in bytes that the
    /// process holds through its open DRM devices, from the kernel's fdinfo
    /// accounting; None when it has no DRM client. Several descriptors can
    /// share one client, which counts once. `clients` is scratch space.
    pub fn gpu(&mut self, pid: u32, clients: &mut Vec<u64>) -> Option<(u64, u64)> {
        self.path.clear();
        let _ = write!(self.path, "/proc/{pid}/fd");
        let dir = fs::read_dir(&self.path).ok()?;
        clients.clear();
        let (mut time, mut vram) = (0, 0);
        for entry in dir.flatten() {
            if !fs::read_link(entry.path()).is_ok_and(|target| target.starts_with("/dev/dri")) {
                continue;
            }
            let name = entry.file_name();
            let Some(fd) = name.to_str() else {
                continue;
            };
            self.path.clear();
            let _ = write!(self.path, "/proc/{pid}/fdinfo/{fd}");
            if !self.read() {
                continue;
            }
            let (mut client, mut engines, mut memory) = (None, 0u64, 0u64);
            for line in String::from_utf8_lossy(&self.raw).lines() {
                let Some((key, value)) = line.split_once(':') else {
                    continue;
                };
                let mut value = value.split_ascii_whitespace();
                let Some(n) = value.next().and_then(|n| n.parse::<u64>().ok()) else {
                    continue;
                };
                match key {
                    "drm-client-id" => client = Some(n),
                    "drm-memory-vram" => {
                        memory = n * match value.next() {
                            Some("KiB") => 1 << 10,
                            Some("MiB") => 1 << 20,
                            _ => 1,
                        }
                    }
                    // drm-engine-gfx, -compute, -dec, ...: busy time in ns.
                    _ if key.starts_with("drm-engine-") && value.next() == Some("ns") => engines += n,
                    _ => {}
                }
            }
            if let Some(client) = client.filter(|c| !clients.contains(c)) {
                clients.push(client);
                time += engines;
                vram += memory;
            }
        }
        (!clients.is_empty()).then_some((time, vram))
    }
}

// Battery

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Charge {
    Charging,
    Discharging,
    /// Full, or plugged in and holding its level.
    Idle,
}

#[derive(Clone, Copy, Debug)]
pub struct Battery {
    pub percent: f32,
    pub charge: Charge,
    pub watts: Option<f32>,
    /// Seconds until empty while discharging, until full while charging.
    pub seconds: Option<u64>,
}

/// The system battery under `root` (/sys/class/power_supply), skipping the
/// batteries of mice and other peripherals.
pub fn battery_dir(root: &Path) -> Option<PathBuf> {
    fs::read_dir(root).ok()?.flatten().map(|e| e.path()).find(|dir| {
        text(dir.join("type")).as_deref() == Some("Battery")
            && text(dir.join("scope")).as_deref() != Some("Device")
            && dir.join("capacity").exists()
    })
}

pub fn battery(dir: &Path) -> Option<Battery> {
    let value = |file: &str| num::<f64>(dir.join(file));
    let charge = match text(dir.join("status")).as_deref() {
        Some("Charging") => Charge::Charging,
        Some("Discharging") => Charge::Discharging,
        _ => Charge::Idle,
    };
    // Batteries report energy (µWh, µW) or charge (µAh, µA); either pair
    // gives hours when divided.
    let (now, full, rate) = match value("energy_now") {
        Some(now) => (Some(now), value("energy_full"), value("power_now")),
        None => (value("charge_now"), value("charge_full"), value("current_now")),
    };
    let watts = value("power_now")
        .or_else(|| Some(value("current_now")? * value("voltage_now")? / 1e6))
        .map(|microwatts| (microwatts.abs() / 1e6) as f32);
    let rate = rate.map(f64::abs).filter(|r| *r > 0.0);
    let hours = match charge {
        Charge::Discharging => Some(now? / rate?),
        Charge::Charging => Some((full? - now?).max(0.0) / rate?),
        Charge::Idle => None,
    };
    Some(Battery {
        percent: value("capacity")? as f32,
        charge,
        watts,
        seconds: hours.map(|h| (h * 3600.0) as u64),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn supply(name: &str, files: &[(&str, &str)]) -> PathBuf {
        let root = std::env::temp_dir().join(format!("terminus-taskmanager-{}-{name}", std::process::id()));
        let dir = root.join("BAT0");
        fs::create_dir_all(&dir).unwrap();
        for (file, content) in files {
            fs::write(dir.join(file), format!("{content}\n")).unwrap();
        }
        root
    }

    #[test]
    fn battery_time_left_from_energy_and_from_charge() {
        // 30 Wh left at 15 W: two hours.
        let root = supply(
            "energy",
            &[
                ("type", "Battery"),
                ("capacity", "60"),
                ("status", "Discharging"),
                ("energy_now", "30000000"),
                ("energy_full", "50000000"),
                ("power_now", "15000000"),
            ],
        );
        let b = battery(&battery_dir(&root).unwrap()).unwrap();
        assert_eq!((b.percent, b.charge, b.seconds), (60.0, Charge::Discharging, Some(7200)));
        assert_eq!(b.watts, Some(15.0));
        fs::remove_dir_all(root).unwrap();

        // 1 Ah missing at 2 A: half an hour to full; 2 A at 12 V is 24 W.
        let root = supply(
            "charge",
            &[
                ("type", "Battery"),
                ("capacity", "75"),
                ("status", "Charging"),
                ("charge_now", "3000000"),
                ("charge_full", "4000000"),
                ("current_now", "2000000"),
                ("voltage_now", "12000000"),
            ],
        );
        let b = battery(&battery_dir(&root).unwrap()).unwrap();
        assert_eq!((b.charge, b.seconds, b.watts), (Charge::Charging, Some(1800), Some(24.0)));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn peripheral_batteries_are_not_the_system_battery() {
        let root = supply("mouse", &[("type", "Battery"), ("scope", "Device"), ("capacity", "40")]);
        assert!(battery_dir(&root).is_none());
        fs::remove_dir_all(root).unwrap();
    }
}
