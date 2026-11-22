//! The cpu and gpu boxes: a device's info box beside the history of its load.

use super::common::{chart, click, click_ends, clock, reading, size, span, truncated};
use crate::app::{pct, App, Hist, Hit, Hits, INTERVAL_STEP_MS};
use crate::draw::{bold, fg, frame, graph, hline, inner, meter, put, put_right, Bar};
use crate::gpu::{Gpu, GpuSample};
use crate::sys::{Battery, Charge};
use crate::theme::Theme;
use ratatui::{buffer::Buffer, layout::Rect};
use std::fmt::Write as _;

/// Charge, then the time to empty or to full, then the draw; with the
/// glyph for the direction of the charge.
fn battery_summary(battery: &Battery) -> (&'static str, String) {
    let mut text = format!(" {:.0}%", battery.percent);
    if let Some(secs) = battery.seconds {
        let _ = write!(text, " {}:{:02}", secs / 3600, secs / 60 % 60);
    }
    if let Some(watts) = battery.watts {
        let _ = write!(text, " {watts:.1}W");
    }
    let glyph = match battery.charge {
        Charge::Charging => "▲",
        Charge::Discharging => "▼",
        Charge::Idle => "■",
    };
    (glyph, text)
}

/// The cpu box's title row: the update interval, pause, the battery.
fn cpu_title_bar(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
    let interval = format!(" {}ms ", app.interval_ms);
    let mut bar = Bar::top(buf, r, t.box_cpu);
    let _ = bar.left(&[("cpu", bold(t.title))]);
    let steps = bar.right(&[("-", fg(t.hi)), (&interval, fg(t.fg)), ("+", fg(t.hi))]);
    click_ends(hits, steps, (1, Hit::Interval(-INTERVAL_STEP_MS)), (1, Hit::Interval(INTERVAL_STEP_MS)));
    let paused = if app.paused { bold(t.title) } else { fg(t.fg) };
    click(hits, bar.right(&[("p", fg(t.hi)), (if app.paused { "aused" } else { "ause" }, paused)]), 'p');
    if let Some(battery) = &app.battery {
        let (glyph, text) = battery_summary(battery);
        bar.right(&[("BAT", bold(t.title)), (glyph, fg(t.hi)), (&text, fg(t.fg))]);
    }
}

/// How the cores fill the cpu info box: one row for the total, then the
/// cores in as many columns as its height asks for, load average on the
/// last row.
struct CoreGrid {
    /// Width of the info box, border included.
    box_w: u16,
    cols: u16,
    rows: u16,
    /// Width of one core's cell, without the gap before the next.
    cell_w: u16,
}

impl CoreGrid {
    /// For `core_count` cores in a cpu box with `iw` by `ih` cells inside.
    fn new(core_count: usize, iw: u16, ih: u16) -> CoreGrid {
        let n = core_count.max(1) as u16;
        let rows_free = ih.saturating_sub(4).max(1);
        let mut cols = n.div_ceil(rows_free);
        // Wide boxes give the core graphs up to twenty cells each.
        let roomy = (cols * 30 + 1).min(iw * 40 / 100);
        let want = ((cols * 17 + cols - 1).max(30) + 2).max(roomy);
        let mut box_w = want.min(iw.saturating_sub(10));
        if box_w < 30 {
            box_w = want.min(iw);
        }
        let inner_w = box_w - 2;
        let mut cell_w = (inner_w + 1) / cols;
        if cell_w < 10 {
            cols = ((inner_w + 1) / 10).max(1);
            cell_w = (inner_w + 1) / cols;
        }
        CoreGrid { box_w, cols, rows: n.div_ceil(cols).min(rows_free), cell_w: cell_w - 1 }
    }
}

/// A device's name and, at the right, one of its clocks: the title of its
/// info box `sub`.
fn device_title(buf: &mut Buffer, sub: Rect, name: &str, clock: Option<&str>, t: &Theme) {
    let room = (sub.width - 2).saturating_sub(clock.map_or(0, |c| c.len() as u16 + 2) + 3);
    let mut bar = Bar::top(buf, sub, t.div);
    bar.left(&[(truncated(name, room), bold(t.title))]);
    if let Some(clock) = clock {
        bar.right(&[(clock, fg(t.fg))]);
    }
}

/// A temperature with its history and the reading at which it is critical.
struct Temperature<'a> {
    celsius: f32,
    history: &'a Hist,
    critical: f32,
}

/// The row that opens a device's info box: `LABEL ■■■■ 37%`, then its
/// temperature as a small graph and a number when there is one. `at` is the
/// box's left column, the row and the inner width.
fn load_row(buf: &mut Buffer, at: (u16, u16, u16), label: &str, percent: f32, temp: Option<Temperature>, t: &Theme) {
    let (x0, y, inner_w) = at;
    let temp_w = if temp.is_some() { 11 } else { 0 };
    let meter_w = inner_w.saturating_sub(4 + 5 + temp_w);
    put(buf, x0, y, label, bold(t.fg));
    meter(buf, x0 + 4, y, meter_w, percent, &t.load, t.meter_bg);
    let x = put(buf, x0 + 4 + meter_w, y, &format!("{percent:>4.0}%"), reading(t, percent));
    if let Some(Temperature { celsius, history, critical }) = temp {
        graph(buf, Rect::new(x + 1, y, 5, 1), history.data(), critical, &t.load, t.inactive, false);
        put(buf, x + 6, y, &format!("{celsius:>3.0}°C"), reading(t, celsius * 100.0 / critical));
    }
}

/// The history of a percentage as a mirrored chart; an area too narrow for
/// one is left empty. Returns where the graphs went.
fn percent_chart(buf: &mut Buffer, area: Rect, history: &Hist, t: &Theme) -> Rect {
    if area.width < 4 {
        return area;
    }
    chart(buf, area, [(history.data(), 100.0, "100%", "50%", &t.load); 2], t)
}

fn frequency(mhz: f32) -> String {
    if mhz >= 1000.0 {
        format!("{:.1} GHz", mhz / 1000.0)
    } else {
        format!("{mhz:.0} MHz")
    }
}

/// The cpu info box `sub`: the total with the temperature, one cell per
/// core, the load average and the package power.
fn cpu_info(buf: &mut Buffer, sub: Rect, grid: &CoreGrid, app: &App, t: &Theme) {
    frame(buf, sub, t.div);
    let (x0, y0, inner_w) = (sub.x + 1, sub.y, sub.width - 2);
    device_title(buf, sub, &app.cpu_name, app.cpu_mhz.map(frequency).as_deref(), t);
    let temp = app.cpu_temp.filter(|_| inner_w >= 30).map(|celsius| Temperature {
        celsius,
        history: &app.cpu_temp_hist,
        critical: app.cpu_temp_crit,
    });
    load_row(buf, (x0, y0 + 1, inner_w), "CPU", app.cpu_pct, temp, t);

    let graph_w = grid.cell_w.saturating_sub(9).min(20);
    for (i, core) in app.cores.iter().enumerate().take((grid.cols * grid.rows).into()) {
        let i = i as u16;
        let (x, y) = (x0 + (i / grid.rows) * (grid.cell_w + 1), y0 + 2 + i % grid.rows);
        put(buf, x, y, &format!("C{i}"), bold(t.fg));
        graph(buf, Rect::new(x + 4, y, graph_w, 1), core.hist.data(), 100.0, &t.load, t.inactive, false);
        put(buf, x + 4 + graph_w, y, &format!("{:>4.0}%", core.pct), fg(t.load.at(core.pct)));
    }
    if sub.height >= 5 && inner_w >= 24 {
        let y = sub.bottom() - 2;
        let load = format!("{} {} {}", app.load[0], app.load[1], app.load[2]);
        let end = sub.right() - 1;
        put_right(buf, end, y, &load, fg(t.fg));
        put_right(buf, end - load.len() as u16 - 1, y, "Load avg:", bold(t.fg));
        // Package power, when the machine lets us read the energy counter.
        if let Some(watts) = app.cpu_watts {
            let text = format!("{watts:.1}W");
            if (text.len() + load.len() + 11) as u16 <= inner_w {
                put(buf, x0, y, &text, fg(t.fg));
            }
        }
    }
}

pub(super) fn cpu_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
    frame(buf, r, t.box_cpu);
    cpu_title_bar(buf, r, app, t, hits);
    let (ix, iy, iw, ih) = inner(r);
    if iw < 14 || ih < 3 {
        return;
    }
    let grid = CoreGrid::new(app.cores.len(), iw, ih);
    cpu_info(buf, Rect::new(ix + iw - grid.box_w, iy, grid.box_w, ih), &grid, app, t);
    let area = percent_chart(buf, Rect::new(ix, iy, iw - grid.box_w, ih), &app.cpu_hist, t);
    let up = format!("up {}", clock(app.uptime));
    span(buf, r, area, t.box_cpu, app, t).right(&[(&up, fg(t.graph_text))]);
}

fn mhz_text(mhz: Option<u32>) -> Option<String> {
    mhz.map(|m| format!("{m} MHz"))
}

/// The fan's share of its top speed and how to say it. The tachometer
/// cannot be read while the fan stands still.
fn fan_reading(g: &GpuSample, gpu: &Gpu) -> Option<(f32, String)> {
    let part = g.fan_pct.or(g.fan_rpm.map(|rpm| rpm as f32 * 100.0 / gpu.fan_max))?;
    let value = match g.fan_rpm {
        Some(rpm) => format!("{rpm}rpm"),
        None if part < 1.0 => "off".to_string(),
        None => format!("{part:.0}%"),
    };
    Some((part, value))
}

/// The video memory block of the gpu info box `sub` from row `y` down: a
/// ruled title, the total, dedicated memory in use and the system memory
/// lent to the card (GTT) while a row is left for them, then the graph.
fn gpu_memory(buf: &mut Buffer, sub: Rect, mut y: u16, (used, total): (u64, u64), app: &App, t: &Theme) {
    let g = &app.gpu_now;
    let (x0, inner_w, end) = (sub.x + 1, sub.width - 2, sub.bottom() - 1);
    put(buf, sub.x, y, "├", fg(t.div));
    hline(buf, x0, y, inner_w, t.div);
    put(buf, sub.right() - 1, y, "┤", fg(t.div));
    let mut bar = Bar::new(buf, y, sub.x + 1, sub.right() - 1, t.div, false);
    bar.left(&[("vram", bold(t.title))]);
    if let Some(mem) = mhz_text(g.mem_mhz) {
        bar.right(&[(&mem, fg(t.fg))]);
    }
    y += 1;
    put(buf, x0, y, "Total:", bold(t.fg));
    put_right(buf, x0 + inner_w, y, &size(total), fg(t.fg));
    let shared = g.gtt_used.zip(g.gtt_total).filter(|_| y + 3 < end);
    let rows = [("Used:", Some((used, total)), &t.load), ("Shared:", shared, &t.cached)];
    for (label, value, gradient) in rows {
        let Some((part, whole)) = value else {
            continue;
        };
        y += 1;
        let share = pct(part, whole);
        put(buf, x0, y, label, bold(t.fg));
        let style = if label == "Used:" { reading(t, share) } else { fg(t.fg) };
        let x = put(buf, x0 + 7, y, &format!("{share:>4.0}% "), style);
        meter(buf, x, y, (x0 + inner_w).saturating_sub(x + 9), share, gradient, t.meter_bg);
        put_right(buf, x0 + inner_w, y, &size(part), fg(t.fg));
    }
    y += 1;
    if y < end {
        graph(buf, Rect::new(x0, y, inner_w, end - y), app.vram_hist.data(), 100.0, &t.load, t.inactive, false);
    }
}

/// The gpu info box `sub`: the load with the temperature, power and fan
/// meters, and the video memory block when there are rows for it.
fn gpu_info(buf: &mut Buffer, sub: Rect, gpu: &Gpu, app: &App, t: &Theme) {
    frame(buf, sub, t.div);
    let g = &app.gpu_now;
    let (x0, inner_w, end) = (sub.x + 1, sub.width - 2, sub.bottom() - 1);
    device_title(buf, sub, &gpu.name, mhz_text(g.core_mhz).as_deref(), t);
    let temp = g.temp.map(|celsius| Temperature { celsius, history: &app.gpu_temp_hist, critical: gpu.temp_crit });
    load_row(buf, (x0, sub.y + 1, inner_w), "GPU", g.util, temp, t);

    let power = g.power.map(|watts| ("PWR", watts * 100.0 / gpu.power_cap, format!("{watts:.1}W")));
    let fan = fan_reading(g, gpu).map(|(part, value)| ("FAN", part, value));
    let mut y = sub.y + 2;
    for (label, part, value) in [power, fan].into_iter().flatten() {
        if y >= end {
            break;
        }
        let value_w = value.chars().count() as u16 + 1;
        put(buf, x0, y, label, bold(t.fg));
        meter(buf, x0 + 4, y, inner_w.saturating_sub(4 + value_w), part, &t.load, t.meter_bg);
        put_right(buf, x0 + inner_w, y, &value, reading(t, part));
        y += 1;
    }
    if let (Some(used), Some(total), true) = (g.vram_used, g.vram_total, y + 2 < end) {
        gpu_memory(buf, sub, y, (used, total), app, t);
    }
}

pub(super) fn gpu_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme) {
    frame(buf, r, t.box_cpu);
    let _ = Bar::top(buf, r, t.box_cpu).left(&[("gpu", bold(t.title))]);
    let (ix, iy, iw, ih) = inner(r);
    let Some(gpu) = &app.gpu else {
        return;
    };
    if iw < 14 || ih < 3 {
        return;
    }
    let mut box_w = (iw * 42 / 100).clamp(32, 44);
    if iw < box_w + 10 {
        box_w = iw;
    }
    gpu_info(buf, Rect::new(ix + iw - box_w, iy, box_w, ih), gpu, app, t);
    let area = percent_chart(buf, Rect::new(ix, iy, iw - box_w, ih), &app.gpu_hist, t);
    span(buf, r, area, t.box_cpu, app, t);
}
