//! Palette. Terminus-Bar renders the wallpaper palette into a btop theme
//! (`btop/themes/terminusbar.theme`) on every scheme and light/dark change;
//! reading that file keeps this monitor and btop on the same colours. Without
//! it, the same roles are derived from the Terminus-Est settings file.
//!
//! Loads are the exception: btop fades every gradient from one palette colour
//! to another, which says little about how high a reading is. Here a load
//! keeps the title colour while calm and turns amber, then red, as it climbs.

use ratatui::style::Color;
use std::{collections::HashMap, env, fs, path::PathBuf, time::SystemTime};

type Rgb = (u8, u8, u8);
type Palette = HashMap<String, Rgb>;

/// Percent from which a load reads as high, and as critical.
const HIGH: usize = 75;
const CRITICAL: usize = 90;

/// The colour of a meter or graph at every percent.
pub struct Gradient([Color; 101]);

impl Gradient {
    /// One colour throughout, for readings where more is not worse.
    fn flat(c: Rgb) -> Gradient {
        Gradient([color(c); 101])
    }

    /// `calm`, then `warn` from `HIGH` and `danger` from `CRITICAL`.
    fn load(calm: Rgb, warn: Rgb, danger: Rgb) -> Gradient {
        let mut steps = [color(calm); 101];
        steps[HIGH..CRITICAL].fill(color(warn));
        steps[CRITICAL..].fill(color(danger));
        Gradient(steps)
    }

    fn step(pct: f32) -> usize {
        pct.clamp(0.0, 100.0).round() as usize
    }

    pub fn at(&self, pct: f32) -> Color {
        self.0[Gradient::step(pct)]
    }

    /// The colour of a high or critical load; None while it is calm.
    pub fn alarm(&self, pct: f32) -> Option<Color> {
        let step = Gradient::step(pct);
        (step >= HIGH).then(|| self.0[step])
    }
}

pub struct Theme {
    pub fg: Color,
    pub title: Color,
    pub hi: Color,
    pub selected_bg: Color,
    pub selected_fg: Color,
    pub inactive: Color,
    pub graph_text: Color,
    pub meter_bg: Color,
    pub box_cpu: Color,
    pub box_mem: Color,
    pub box_net: Color,
    pub box_proc: Color,
    pub div: Color,
    /// Anything where high means trouble: cpu, gpu, heat, used space.
    pub load: Gradient,
    pub available: Gradient,
    pub cached: Gradient,
    pub free: Gradient,
    pub download: Gradient,
    pub upload: Gradient,
}

fn parse_color(s: &str) -> Option<Rgb> {
    let s = s.strip_prefix('#')?;
    match s.len() {
        6 => {
            let v = u32::from_str_radix(s, 16).ok()?;
            Some(((v >> 16) as u8, (v >> 8) as u8, v as u8))
        }
        // btop shorthand: two hex digits are a grey.
        2 => u8::from_str_radix(s, 16).ok().map(|g| (g, g, g)),
        _ => None,
    }
}

fn mix(a: Rgb, b: Rgb, t: f32) -> Rgb {
    let m = |x: u8, y: u8| (x as f32 + (y as f32 - x as f32) * t).round() as u8;
    (m(a.0, b.0), m(a.1, b.1), m(a.2, b.2))
}

fn color(c: Rgb) -> Color {
    Color::Rgb(c.0, c.1, c.2)
}

fn home() -> PathBuf {
    PathBuf::from(env::var_os("HOME").unwrap_or_default())
}

fn config_dir() -> PathBuf {
    env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| home().join(".config"))
}

fn bar_theme_path() -> PathBuf {
    config_dir().join("btop/themes/terminusbar.theme")
}

/// The theme Terminus-Bar renders for btop: lines of `theme[key]="#rrggbb"`.
fn bar_theme() -> Option<Palette> {
    let text = fs::read_to_string(bar_theme_path()).ok()?;
    let palette: Palette = text
        .lines()
        .filter_map(|line| {
            let (key, value) = line.trim().strip_prefix("theme[")?.split_once("]=")?;
            Some((key.to_string(), parse_color(value.trim().trim_matches('"'))?))
        })
        .collect();
    palette.contains_key("main_fg").then_some(palette)
}

/// The same roles from `settings.toml` `[appearance]` (lib/tokens.py's values
/// when the file or a key is missing, its LIGHT palette in light mode).
fn settings_theme() -> Palette {
    let light = fs::read_to_string(home().join(".local/state/terminus-est/theme-mode"))
        .is_ok_and(|s| s.trim() == "light");
    let [mut bg, mut fg, mut muted, mut accent, mut alert] = if light {
        ["#fbf7f0", "#000000", "#595959", "#0031a9", "#a60000"]
    } else {
        ["#0b0b0b", "#ececec", "#8c8c8c", "#ffb3b0", "#e8a07a"]
    }
    .map(|c| parse_color(c).unwrap_or_default());

    if !light {
        let appearance = fs::read_to_string(config_dir().join("terminus-est/settings.toml"))
            .ok()
            .and_then(|text| text.parse::<toml::Table>().ok())
            .and_then(|mut table| table.remove("appearance"));
        if let Some(appearance) = appearance.as_ref().and_then(|v| v.as_table()) {
            for (key, slot) in [
                ("background", &mut bg),
                ("foreground", &mut fg),
                ("muted", &mut muted),
                ("accent", &mut accent),
                ("alert", &mut alert),
            ] {
                if let Some(c) = appearance.get(key).and_then(|v| v.as_str()).and_then(parse_color) {
                    *slot = c;
                }
            }
        }
    }

    let mut p = Palette::new();
    let mut set = |key: &str, c: Rgb| {
        p.insert(key.to_string(), c);
    };
    set("main_bg", bg);
    set("main_fg", fg);
    set("title", accent);
    set("hi_fg", alert);
    set("selected_bg", mix(bg, accent, 0.3));
    set("selected_fg", fg);
    set("inactive_fg", muted);
    set("div_line", mix(bg, muted, 0.4));
    for name in ["cpu_box", "mem_box", "net_box", "proc_box"] {
        set(name, mix(bg, muted, 0.6));
    }
    p
}

impl Theme {
    /// When the bar's theme file was last written; a new value means
    /// `load` again.
    pub fn changed() -> Option<SystemTime> {
        fs::metadata(bar_theme_path()).and_then(|m| m.modified()).ok()
    }

    pub fn load() -> Theme {
        let p = bar_theme().unwrap_or_else(settings_theme);
        let get = |key: &str| p.get(key).copied();
        let fg = get("main_fg").unwrap_or((0xec, 0xec, 0xec));
        let title = get("title").unwrap_or(fg);
        let hi = get("hi_fg").unwrap_or(fg);
        let inactive = get("inactive_fg").unwrap_or(fg);
        let div = get("div_line").unwrap_or(inactive);
        let outline = |name: &str| color(get(name).unwrap_or(div));
        // Amber and red that stand out from the background either way.
        let (r, g, b) = get("main_bg").unwrap_or_default();
        let light = r as u32 * 299 + g as u32 * 587 + b as u32 * 114 > 128_000;
        let (warn, danger) = if light {
            ((0xa8, 0x5a, 0x00), (0xc4, 0x1c, 0x1c))
        } else {
            ((0xff, 0xc1, 0x4d), (0xff, 0x45, 0x45))
        };
        Theme {
            fg: color(fg),
            title: color(title),
            hi: color(hi),
            selected_bg: color(get("selected_bg").unwrap_or(div)),
            selected_fg: color(get("selected_fg").unwrap_or(fg)),
            inactive: color(inactive),
            graph_text: color(get("graph_text").unwrap_or(inactive)),
            // btop falls back to inactive_fg, which is nearly the fill colour
            // in this palette; the divider colour keeps meters readable.
            meter_bg: color(get("meter_bg").unwrap_or(div)),
            box_cpu: outline("cpu_box"),
            box_mem: outline("mem_box"),
            box_net: outline("net_box"),
            box_proc: outline("proc_box"),
            div: color(div),
            load: Gradient::load(title, warn, danger),
            // Room left and traffic take the highlight colour, so they
            // never read as a load.
            available: Gradient::flat(hi),
            cached: Gradient::flat(get("proc_misc").unwrap_or(title)),
            free: Gradient::flat(hi),
            download: Gradient::flat(hi),
            upload: Gradient::flat(title),
        }
    }
}
