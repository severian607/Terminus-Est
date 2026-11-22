//! AMD GPU telemetry from the kernel's amdgpu sysfs files. No ROCm needed.

use crate::sys::{num, text};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

pub struct Gpu {
    dir: PathBuf,
    hwmon: Option<PathBuf>,
    pub name: String,
    /// Power limit in watts, the top of the power meter.
    pub power_cap: f32,
    pub temp_crit: f32,
    pub fan_max: f32,
}

#[derive(Default, Clone, Copy)]
pub struct GpuSample {
    pub util: f32,
    pub vram_used: Option<u64>,
    pub vram_total: Option<u64>,
    pub temp: Option<f32>,
    pub power: Option<f32>,
    pub core_mhz: Option<u32>,
    pub mem_mhz: Option<u32>,
    pub fan_rpm: Option<u32>,
    /// Percent of full fan drive.
    pub fan_pct: Option<f32>,
    /// System memory lent to the card.
    pub gtt_used: Option<u64>,
    pub gtt_total: Option<u64>,
}

impl Gpu {
    pub fn detect() -> Option<Gpu> {
        let mut cards: Vec<PathBuf> = fs::read_dir("/sys/class/drm")
            .ok()?
            .flatten()
            .filter(|e| {
                let name = e.file_name();
                let name = name.to_string_lossy();
                name.strip_prefix("card").is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
            })
            .map(|e| e.path().join("device"))
            .filter(|d| d.join("gpu_busy_percent").exists())
            .collect();
        cards.sort();
        let dir = cards.into_iter().next()?;
        let hwmon = fs::read_dir(dir.join("hwmon")).ok().and_then(|d| d.flatten().next()).map(|e| e.path());
        let hw = |file: &str| hwmon.as_ref().and_then(|h| num::<f32>(h.join(file)));
        Some(Gpu {
            name: name_of(&dir),
            power_cap: hw("power1_cap").map_or(250.0, |w| w / 1e6),
            temp_crit: hw("temp1_crit").map_or(100.0, |t| t / 1e3),
            fan_max: hw("fan1_max").unwrap_or(4000.0),
            dir,
            hwmon,
        })
    }

    /// A card that reads nothing, for rendering tests.
    #[cfg(test)]
    pub fn fixture() -> Gpu {
        Gpu {
            dir: PathBuf::new(),
            hwmon: None,
            name: "Fixture GPU 9000".to_string(),
            power_cap: 200.0,
            temp_crit: 100.0,
            fan_max: 4000.0,
        }
    }

    pub fn sample(&self) -> GpuSample {
        let dev = |file: &str| num::<u64>(self.dir.join(file));
        let hw = |file: &str| self.hwmon.as_ref().and_then(|h| num::<u64>(h.join(file)));
        GpuSample {
            util: dev("gpu_busy_percent").unwrap_or(0) as f32,
            vram_used: dev("mem_info_vram_used"),
            vram_total: dev("mem_info_vram_total"),
            temp: hw("temp1_input").map(|t| t as f32 / 1e3),
            power: hw("power1_average").or_else(|| hw("power1_input")).map(|p| p as f32 / 1e6),
            core_mhz: hw("freq1_input").map(|f| (f / 1_000_000) as u32),
            mem_mhz: hw("freq2_input").map(|f| (f / 1_000_000) as u32),
            // amdgpu refuses the tachometer read while the fan stands still.
            fan_rpm: hw("fan1_input").map(|rpm| rpm as u32),
            fan_pct: hw("pwm1")
                .zip(hw("pwm1_max"))
                .filter(|p| p.1 > 0)
                .map(|(pwm, max)| pwm as f32 * 100.0 / max as f32),
            gtt_used: dev("mem_info_gtt_used"),
            gtt_total: dev("mem_info_gtt_total"),
        }
    }
}

/// The marketing name from lspci ("Radeon RX 580 2048SP"), else the PCI id.
fn name_of(dir: &Path) -> String {
    let slot = text(dir.join("uevent"))
        .and_then(|u| u.lines().find_map(|l| l.strip_prefix("PCI_SLOT_NAME=").map(String::from)));
    let from_lspci = slot.and_then(|slot| {
        let out = Command::new("lspci").args(["-mm", "-s", &slot]).output().ok()?;
        let line = String::from_utf8_lossy(&out.stdout);
        let device = line.split('"').nth(5)?;
        // "Polaris 20 XL/XTR [Radeon RX 580 2048SP / RX 590 GME]"
        let name = match (device.find('['), device.rfind(']')) {
            (Some(a), Some(b)) if b > a => &device[a + 1..b],
            _ => device,
        };
        let name = name.split(" / ").next().unwrap_or(name).trim();
        (!name.is_empty()).then(|| name.to_string())
    });
    from_lspci.unwrap_or_else(|| {
        let id = |file: &str| text(dir.join(file)).unwrap_or_default().trim_start_matches("0x").to_string();
        format!("AMD GPU ({}:{})", id("vendor"), id("device"))
    })
}
