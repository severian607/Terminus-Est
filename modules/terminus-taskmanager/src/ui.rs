//! The boxes, laid out as btop does it except that the GPU sits left of the
//! CPU instead of under it:
//!
//!   gpu0 | cpu
//!   mem  | proc
//!   net  |

use crate::app::{App, Ask, Dialog, Disk, Hit, Hits, Sort, SIGNAL_ROWS};
use crate::draw::{bold, fg, frame, gauge, graph, half_line, hline, inner, meter, put, put_right, putn, vline, Bar};
use crate::sys::{Charge, SIGNALS};
use crate::theme::{Gradient, Theme};
use ratatui::{
    buffer::Buffer,
    crossterm::event::KeyCode,
    layout::Rect,
    style::{Color, Modifier, Style},
    Frame,
};
use std::borrow::Cow;

const BYTES: [&str; 5] = ["Byte", "KiB", "MiB", "GiB", "TiB"];
const SHORT: [&str; 5] = ["B", "K", "M", "G", "T"];
const BITS: [&str; 5] = ["bps", "Kibps", "Mibps", "Gibps", "Tibps"];

/// Three significant digits and a binary unit, as btop prints sizes.
fn human(value: f64, units: &[&str; 5], short: bool) -> String {
    let (mut v, mut unit) = (value, 0);
    while v >= 1000.0 && unit < 4 {
        v /= 1024.0;
        unit += 1;
    }
    let decimals = match (unit, short) {
        (0, _) => 0,
        (_, false) if v < 10.0 => 2,
        (_, false) if v < 100.0 => 1,
        (_, true) if v < 10.0 => 1,
        _ => 0,
    };
    let gap = if short { "" } else { " " };
    // btop truncates instead of rounding: 15.49 GiB reads 15.4.
    let scale = 10f64.powi(decimals as i32);
    let v = (v * scale).floor() / scale;
    format!("{v:.decimals$}{gap}{}", units[unit])
}

fn size(bytes: u64) -> String {
    human(bytes as f64, &BYTES, false)
}

/// `04:12:33`, with the days in front once there are any.
fn clock(secs: u64) -> String {
    let (d, h, m, s) = (secs / 86400, secs / 3600 % 24, secs / 60 % 60, secs % 60);
    if d > 0 {
        format!("{d}d {h:02}:{m:02}:{s:02}")
    } else {
        format!("{h:02}:{m:02}:{s:02}")
    }
}

fn pct(part: u64, whole: u64) -> f32 {
    if whole == 0 {
        0.0
    } else {
        part as f32 * 100.0 / whole as f32
    }
}

fn truncated(s: &str, max: u16) -> &str {
    s.char_indices().nth(max as usize).map_or(s, |(i, _)| &s[..i])
}

/// Makes a title clickable: a click presses `key`.
fn click(hits: &mut Hits, title: Option<Rect>, key: char) {
    if let Some(area) = title {
        hits.push((area, Hit::Key(KeyCode::Char(key))));
    }
}

/// Makes the first and last cells of a title clickable, each `(width, hit)`.
fn click_ends(hits: &mut Hits, title: Option<Rect>, first: (u16, Hit), last: (u16, Hit)) {
    if let Some(area) = title {
        hits.push((Rect { width: first.0, ..area }, first.1));
        hits.push((Rect { x: area.right() - last.0, width: last.0, ..area }, last.1));
    }
}

pub fn draw(f: &mut Frame, app: &mut App, t: &Theme) {
    // Click targets belong to the frame they were drawn in.
    let mut hits = std::mem::take(&mut app.hits);
    hits.clear();
    app.proc_area = Rect::default();
    let area = f.area();
    layout(f.buffer_mut(), area, app, t, &mut hits);
    app.hits = hits;
}

/// Height of the gpu/cpu band above other boxes: the height nearest three
/// tenths of the screen at which the cores, in some number of columns, fill
/// the cpu info box exactly.
fn top_height(height: u16, cpu_w: u16, cores: usize) -> u16 {
    let n = cores.max(1) as u16;
    (1..=n)
        .filter(|cols| cols * 18 + 13 <= cpu_w)
        .map(|cols| n.div_ceil(cols) + 6)
        .filter(|h| (10..=20).contains(h) && h + 12 <= height)
        .min_by_key(|h| h.abs_diff(height * 3 / 10))
        .unwrap_or((height * 36 / 100).clamp(11, 20))
}

fn layout(buf: &mut Buffer, a: Rect, app: &mut App, t: &Theme, hits: &mut Hits) {
    let note = |buf: &mut Buffer, msg: &str| {
        put(buf, a.width.saturating_sub(msg.len() as u16) / 2, a.height / 2, msg, fg(t.fg));
    };
    if a.width < 80 || a.height < 24 {
        return note(buf, &format!("Terminal is {}x{}, needs 80x24", a.width, a.height));
    }
    let s = &app.shown;
    let (top, left) = (s.cpu || s.gpu, s.mem || s.net);
    let bottom = left || s.proc;
    if !top && !bottom {
        return note(buf, "Every box is hidden: 1 cpu  2 mem  3 net  4 proc  5 gpu");
    }
    let left_w = a.width * 45 / 100;
    let top_h = match (top, bottom) {
        (false, _) => 0,
        (true, false) => a.height,
        (true, true) => top_height(a.height, a.width - if s.gpu { left_w } else { 0 }, app.cores.len()),
    };
    let (gpu, cpu, mem, net, show_proc) = (s.gpu, s.cpu, s.mem, s.net, s.proc);

    let upper = Rect::new(0, 0, a.width, top_h);
    let gpu_w = if cpu { left_w } else { a.width };
    if gpu {
        gpu_box(buf, Rect { width: gpu_w, ..upper }, app, t);
    }
    if cpu {
        let x = if gpu { gpu_w } else { 0 };
        cpu_box(buf, Rect { x, width: a.width - x, ..upper }, app, t, hits);
    }

    let lower = Rect::new(0, top_h, a.width, a.height - top_h);
    let side = Rect { width: if show_proc { left_w } else { a.width }, ..lower };
    // The memory box is as tall as its disks ask for with a used and a free
    // meter each, within what leaves the net box its three lines per
    // direction and at most 55%.
    let mem_h = match net {
        false => side.height,
        true if side.height >= 21 => {
            let (mut count, mut busy) = (0, 0);
            each_disk(app, &mut |_, _, _, io| {
                count += 1;
                busy += io.is_some() as u16;
            });
            let net_max = (side.height * 55 / 100).max(10);
            (3 * count + busy + 2).clamp(side.height - net_max, side.height - 10)
        }
        true => side.height * 58 / 100,
    };
    if mem {
        mem_box(buf, Rect { height: mem_h, ..side }, app, t);
    }
    if net {
        let y = if mem { side.y + mem_h } else { side.y };
        net_box(buf, Rect { y, height: side.bottom() - y, ..side }, app, t, hits);
    }
    if show_proc {
        let x = if left { left_w } else { 0 };
        proc_box(buf, Rect { x, width: a.width - x, ..lower }, app, t, hits);
    }
    if let Some(dialog) = &app.dialog {
        // A dialog is modal: only its own targets take clicks.
        hits.clear();
        dialog_box(buf, a, dialog, t, hits);
    }
}

/// A reading in the text colour, or bold in its alarm colour once it is high.
fn reading(t: &Theme, pct: f32) -> Style {
    t.load.alarm(pct).map_or(fg(t.fg), bold)
}

/// How far back a graph `cells` wide reaches at the current interval.
fn ago(app: &App, cells: u16) -> String {
    let secs = cells as u64 * 2 * app.interval_ms / 1000;
    match secs {
        0..=89 => format!("{secs}s ago"),
        90..=5399 => format!("{}m ago", (secs + 30) / 60),
        _ => format!("{}h{:02}m ago", secs / 3600, secs / 60 % 60),
    }
}

/// One half of a mirrored chart: its samples, the value at full height,
/// the labels for that value and for half of it, and its colours.
type Half<'a> = (&'a [f32], f32, &'a str, &'a str, &'a Gradient);

/// A mirrored pair of graphs filling `r`, as btop draws them: the first
/// grows up from the middle and the second hangs below it. With room, the
/// scale stands down the left: each half's full value at its outer edge,
/// `0` at the middle, and its half value on a dotted line. Returns where
/// the graphs went.
fn chart(buf: &mut Buffer, r: Rect, halves: [Half; 2], t: &Theme) -> Rect {
    let label_w = halves.iter().map(|h| h.2.chars().count().max(h.3.chars().count())).max().unwrap_or(0) as u16;
    let scaled = r.height >= 4 && r.width >= label_w + 9;
    let area = if scaled { Rect { x: r.x + label_w + 1, width: r.width - label_w - 1, ..r } } else { r };
    let up = r.height.div_ceil(2);
    let parts = [(Rect { height: up, ..area }, false), (Rect { y: r.y + up, height: r.height - up, ..area }, true)];
    for ((data, max, full, half, g), (part, invert)) in halves.into_iter().zip(parts) {
        if scaled {
            let label = |buf: &mut Buffer, y: u16, s: &str| put_right(buf, area.x - 1, y, s, fg(t.graph_text));
            if invert {
                label(buf, part.bottom() - 1, full);
            } else {
                label(buf, part.bottom() - 1, "0");
                label(buf, part.y, full);
            }
            if part.height >= 4 {
                let y = half_line(buf, part, invert, t.div);
                label(buf, y, half);
            }
        }
        graph(buf, part, data, max, g, t.inactive, invert);
    }
    area
}

/// Under a chart, in the box's bottom border: how far back its left edge
/// reaches. Returns the bar for more titles.
fn span<'b>(buf: &'b mut Buffer, r: Rect, graph: Rect, line: Color, app: &App, t: &Theme) -> Bar<'b> {
    let mut bar = Bar::new(buf, r.bottom() - 1, graph.x.max(r.x + 2) - 1, r.right() - 1, line, true);
    if graph.width >= 4 {
        bar.left(&[(&ago(app, graph.width), fg(t.graph_text))]);
    }
    bar
}

/// `├─label──────value─┤` across columns `left..=right`, the way btop
/// separates items inside a box. The line shows through the value's space.
fn rule(buf: &mut Buffer, (left, right): (u16, u16), y: u16, label: &str, value: &str, t: &Theme, ends: (Color, Color)) {
    put(buf, left, y, "├", fg(ends.0));
    hline(buf, left + 1, y, right.saturating_sub(left + 1), t.div);
    put(buf, right, y, "┤", fg(ends.1));
    put(buf, left + 2, y, label, fg(t.fg));
    let mut x = right.saturating_sub(1 + value.chars().count() as u16);
    for word in value.split(' ') {
        x = put(buf, x, y, word, fg(t.fg)) + 1;
    }
}

fn cpu_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
    frame(buf, r, t.box_cpu);
    {
        let interval = format!(" {}ms ", app.interval_ms);
        let mut bar = Bar::top(buf, r, t.box_cpu);
        let _ = bar.left(&[("cpu", bold(t.title))]);
        let steps = bar.right(&[("-", fg(t.hi)), (&interval, fg(t.fg)), ("+", fg(t.hi))]);
        click_ends(hits, steps, (1, Hit::Interval(-100)), (1, Hit::Interval(100)));
        let paused = if app.paused { bold(t.title) } else { fg(t.fg) };
        click(hits, bar.right(&[("p", fg(t.hi)), (if app.paused { "aused" } else { "ause" }, paused)]), 'p');
        if let Some(b) = &app.battery {
            // Charge, then the time to empty or to full, then the draw.
            let mut text = format!(" {:.0}%", b.percent);
            if let Some(secs) = b.seconds {
                text.push_str(&format!(" {}:{:02}", secs / 3600, secs / 60 % 60));
            }
            if let Some(watts) = b.watts {
                text.push_str(&format!(" {watts:.1}W"));
            }
            let state = match b.charge {
                Charge::Charging => "▲",
                Charge::Discharging => "▼",
                Charge::Idle => "■",
            };
            bar.right(&[("BAT", bold(t.title)), (state, fg(t.hi)), (&text, fg(t.fg))]);
        }
    }
    let (ix, iy, iw, ih) = inner(r);
    if iw < 14 || ih < 3 {
        return;
    }

    // The info box: one row for the total, then the cores in as many
    // columns as its height asks for, load average on the last row.
    let n = app.cores.len().max(1) as u16;
    let rows_free = ih.saturating_sub(4).max(1);
    let mut cols = n.div_ceil(rows_free);
    // Wide boxes give the core graphs up to twenty cells each.
    let roomy = (cols * 30 + 1).min(iw * 40 / 100);
    let want = ((cols * 17 + cols - 1).max(30) + 2).max(roomy);
    let mut sub_w = want.min(iw.saturating_sub(10));
    if sub_w < 30 {
        sub_w = want.min(iw);
    }
    let sw = sub_w - 2;
    let mut cell = (sw + 1) / cols;
    if cell < 10 {
        cols = ((sw + 1) / 10).max(1);
        cell = (sw + 1) / cols;
    }
    let cell = cell - 1;
    let rows = n.div_ceil(cols).min(rows_free);
    let sub = Rect::new(ix + iw - sub_w, iy, sub_w, ih);
    frame(buf, sub, t.div);
    {
        let freq = app.cpu_mhz.map(|mhz| {
            if mhz >= 1000.0 {
                format!("{:.1} GHz", mhz / 1000.0)
            } else {
                format!("{mhz:.0} MHz")
            }
        });
        let name = truncated(&app.cpu_name, sw.saturating_sub(freq.as_ref().map_or(0, |f| f.len() as u16 + 2) + 3));
        let mut bar = Bar::top(buf, sub, t.div);
        bar.left(&[(name, bold(t.title))]);
        if let Some(freq) = &freq {
            bar.right(&[(freq, fg(t.fg))]);
        }
    }
    let x0 = sub.x + 1;
    let temp_w = if app.cpu_temp.is_some() && sw >= 30 { 11 } else { 0 };
    let meter_w = sw.saturating_sub(4 + 5 + temp_w);
    put(buf, x0, iy + 1, "CPU", bold(t.fg));
    meter(buf, x0 + 4, iy + 1, meter_w, app.cpu_pct, &t.load, t.meter_bg);
    let x = put(buf, x0 + 4 + meter_w, iy + 1, &format!("{:>4.0}%", app.cpu_pct), reading(t, app.cpu_pct));
    if let (Some(temp), true) = (app.cpu_temp, temp_w > 0) {
        let data = app.cpu_temp_hist.data();
        graph(buf, Rect::new(x + 1, iy + 1, 5, 1), data, app.cpu_temp_crit, &t.load, t.inactive, false);
        put(buf, x + 6, iy + 1, &format!("{temp:>3.0}°C"), reading(t, temp * 100.0 / app.cpu_temp_crit));
    }
    let graph_w = cell.saturating_sub(9).min(20);
    for i in 0..(cols * rows).min(n) {
        let Some(core) = app.cores.get(i as usize) else {
            break;
        };
        let (x, y) = (x0 + (i / rows) * (cell + 1), iy + 2 + i % rows);
        put(buf, x, y, &format!("C{i}"), bold(t.fg));
        graph(buf, Rect::new(x + 4, y, graph_w, 1), core.hist.data(), 100.0, &t.load, t.inactive, false);
        put(buf, x + 4 + graph_w, y, &format!("{:>4.0}%", core.pct), fg(t.load.at(core.pct)));
    }
    if ih >= 5 && sw >= 24 {
        let load = format!("{} {} {}", app.load[0], app.load[1], app.load[2]);
        let end = sub.right() - 1;
        put_right(buf, end, iy + ih - 2, &load, fg(t.fg));
        put_right(buf, end - load.len() as u16 - 1, iy + ih - 2, "Load avg:", bold(t.fg));
        // Package power, when the machine lets us read the energy counter.
        if let Some(watts) = app.cpu_watts {
            let text = format!("{watts:.1}W");
            if (text.len() + load.len() + 11) as u16 <= sw {
                put(buf, x0, iy + ih - 2, &text, fg(t.fg));
            }
        }
    }

    let mut area = Rect::new(ix, iy, iw - sub_w, ih);
    if area.width >= 4 {
        area = chart(buf, area, [(app.cpu_hist.data(), 100.0, "100%", "50%", &t.load); 2], t);
    }
    let up = format!("up {}", clock(app.uptime));
    span(buf, r, area, t.box_cpu, app, t).right(&[(&up, fg(t.graph_text))]);
}

fn gpu_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme) {
    frame(buf, r, t.box_cpu);
    let _ = Bar::top(buf, r, t.box_cpu).left(&[("gpu", bold(t.title))]);
    let (ix, iy, iw, ih) = inner(r);
    let Some(gpu) = &app.gpu else {
        return;
    };
    if iw < 14 || ih < 3 {
        return;
    }
    let g = &app.gpu_now;
    let mut sub_w = (iw * 42 / 100).clamp(32, 44);
    if iw < sub_w + 10 {
        sub_w = iw;
    }
    let sw = sub_w - 2;
    let sub = Rect::new(ix + iw - sub_w, iy, sub_w, ih);
    frame(buf, sub, t.div);
    let clock = |mhz: Option<u32>| mhz.map(|m| format!("{m} MHz"));
    {
        let core = clock(g.core_mhz);
        let name = truncated(&gpu.name, sw.saturating_sub(core.as_ref().map_or(0, |c| c.len() as u16 + 2) + 3));
        let mut bar = Bar::top(buf, sub, t.div);
        bar.left(&[(name, bold(t.title))]);
        if let Some(core) = &core {
            bar.right(&[(core, fg(t.fg))]);
        }
    }
    let x0 = sub.x + 1;
    let end = sub.bottom() - 1;
    let mut y = iy + 1;

    let temp_w = if g.temp.is_some() { 11 } else { 0 };
    let meter_w = sw.saturating_sub(4 + 5 + temp_w);
    put(buf, x0, y, "GPU", bold(t.fg));
    meter(buf, x0 + 4, y, meter_w, g.util, &t.load, t.meter_bg);
    let x = put(buf, x0 + 4 + meter_w, y, &format!("{:>4.0}%", g.util), reading(t, g.util));
    if let Some(temp) = g.temp {
        graph(buf, Rect::new(x + 1, y, 5, 1), app.gpu_temp_hist.data(), gpu.temp_crit, &t.load, t.inactive, false);
        put(buf, x + 6, y, &format!("{temp:>3.0}°C"), reading(t, temp * 100.0 / gpu.temp_crit));
    }
    y += 1;
    let row = |buf: &mut Buffer, y: &mut u16, label: &str, part: f32, value: String| {
        if *y >= end {
            return;
        }
        let value_w = value.chars().count() as u16 + 1;
        put(buf, x0, *y, label, bold(t.fg));
        meter(buf, x0 + 4, *y, sw.saturating_sub(4 + value_w), part, &t.load, t.meter_bg);
        put_right(buf, x0 + sw, *y, &value, reading(t, part));
        *y += 1;
    };
    if let Some(watts) = g.power {
        row(buf, &mut y, "PWR", watts * 100.0 / gpu.power_cap, format!("{watts:.1}W"));
    }
    // The tachometer cannot be read while the fan stands still.
    if let Some(part) = g.fan_pct.or(g.fan_rpm.map(|rpm| rpm as f32 * 100.0 / gpu.fan_max)) {
        let value = match g.fan_rpm {
            Some(rpm) => format!("{rpm}rpm"),
            None if part < 1.0 => "off".to_string(),
            None => format!("{part:.0}%"),
        };
        row(buf, &mut y, "FAN", part, value);
    }

    if let (Some(used), Some(total), true) = (g.vram_used, g.vram_total, y + 2 < end) {
        put(buf, sub.x, y, "├", fg(t.div));
        hline(buf, x0, y, sw, t.div);
        put(buf, sub.right() - 1, y, "┤", fg(t.div));
        {
            let mut bar = Bar::new(buf, y, sub.x + 1, sub.right() - 1, t.div, false);
            bar.left(&[("vram", bold(t.title))]);
            if let Some(mem) = clock(g.mem_mhz) {
                bar.right(&[(&mem, fg(t.fg))]);
            }
        }
        y += 1;
        put(buf, x0, y, "Total:", bold(t.fg));
        put_right(buf, x0 + sw, y, &size(total), fg(t.fg));
        // Dedicated memory, then the system memory lent to the card (GTT)
        // while a row is left for the graph.
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
            meter(buf, x, y, (x0 + sw).saturating_sub(x + 9), share, gradient, t.meter_bg);
            put_right(buf, x0 + sw, y, &size(part), fg(t.fg));
        }
        y += 1;
        if y < end {
            let area = Rect::new(x0, y, sw, end - y);
            graph(buf, area, app.vram_hist.data(), 100.0, &t.load, t.inactive, false);
        }
    }

    let mut area = Rect::new(ix, iy, iw - sub_w, ih);
    if area.width >= 4 {
        area = chart(buf, area, [(app.gpu_hist.data(), 100.0, "100%", "50%", &t.load); 2], t);
    }
    span(buf, r, area, t.box_cpu, app, t);
}

/// What btop draws at the top of a disk's read and write graphs.
const IO_SPEED: f32 = 10.0 * 1024.0 * 1024.0;

type DiskFn<'a> = dyn FnMut(&str, u64, u64, Option<&Disk>) + 'a;

/// Calls `f` with the name, size and used bytes of every disk in the order
/// shown, and the disk itself when it has I/O statistics. btop lists swap
/// as the second disk.
fn each_disk(app: &App, f: &mut DiskFn<'_>) {
    let m = &app.mem;
    let swap = |f: &mut DiskFn| {
        if m.swap_total > 0 {
            f("swap", m.swap_total, m.swap_total - m.swap_free, None);
        }
    };
    for (i, d) in app.disks.iter().filter(|d| d.total > 0).enumerate() {
        if i == 1 {
            swap(f);
        }
        f(&d.name, d.total, d.used, (!d.io.data().is_empty()).then_some(d));
    }
    if app.disks.len() < 2 {
        swap(f);
    }
}

fn mem_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme) {
    frame(buf, r, t.box_mem);
    let _ = Bar::top(buf, r, t.box_mem).left(&[("mem", bold(t.title))]);
    let (ix, iy, iw, ih) = inner(r);
    if iw < 20 || ih < 2 {
        return;
    }
    let m = &app.mem;
    let disks = iw >= 44;
    let mem_w = if disks { iw / 2 } else { iw };
    // Column that closes the memory half: the divider, or the box's border.
    let edge = ix + mem_w;
    if disks {
        put(buf, edge, r.y, "┬", fg(t.box_mem));
        vline(buf, edge, iy, ih, t.div);
        put(buf, edge, r.bottom() - 1, "┴", fg(t.box_mem));
    }
    let edge_color = if disks { t.div } else { t.box_mem };
    let (x0, cw) = (ix + 1, mem_w - 2);

    put(buf, x0, iy, "Total:", bold(t.fg));
    put_right(buf, x0 + cw, iy, &size(m.total), bold(t.fg));
    let items = [
        ("Used:", m.total.saturating_sub(m.available), &app.mem_used, &t.load),
        ("Available:", m.available, &app.mem_available, &t.available),
        ("Cached:", m.cached, &app.mem_cached, &t.cached),
        ("Free:", m.free, &app.mem_free, &t.free),
    ];
    // Rows left over after an even split go to the first items.
    let (per_item, extra) = ((ih - 1) / 4, (ih - 1) % 4);
    let mut y = iy + 1;
    for (i, (label, bytes, hist, gradient)) in items.into_iter().enumerate() {
        if y >= iy + ih {
            break;
        }
        let share = format!("{:>3.0}%", hist.last());
        // Only what is used can run high.
        let style = if i == 0 { reading(t, hist.last()) } else { fg(t.fg) };
        if per_item >= 2 {
            // A ruled label row, then the graph of this item's share.
            let rows = per_item - 1 + ((i as u16) < extra) as u16;
            rule(buf, (r.x, edge), y, label, &size(bytes), t, (t.box_mem, edge_color));
            let area = Rect::new(x0, y + 1, cw.saturating_sub(5), rows);
            graph(buf, area, hist.data(), 100.0, gradient, t.inactive, false);
            put_right(buf, x0 + cw, y + rows, &share, style);
            y += rows + 1;
        } else {
            // No room for graphs: label, share, meter and value on one row.
            let value = size(bytes);
            let meter_w = cw.saturating_sub(10 + 5 + 1 + 9);
            put(buf, x0, y, label, fg(t.fg));
            put(buf, x0 + 10, y, &share, style);
            meter(buf, x0 + 16, y, meter_w, hist.last(), gradient, t.meter_bg);
            put_right(buf, x0 + cw, y, &value, fg(t.fg));
            y += 1;
        }
    }
    if !disks {
        return;
    }

    let (dx, dw) = (edge + 2, iw - mem_w - 3);
    let last = r.right() - 1;
    Bar::new(buf, r.y, edge + 1, last, t.box_mem, false).left(&[("disks", bold(t.title))]);
    // One block per disk: name and size, how busy it is, how full it is.
    // Spare rows first give every disk a free-space meter, as btop does,
    // then graphs of what it reads and writes, then taller busy graphs.
    let (mut count, mut busy) = (0u16, 0u16);
    each_disk(app, &mut |_, _, _, io| {
        count += 1;
        busy += io.is_some() as u16;
    });
    let mut rows = 2 * count + busy;
    let free_row = rows + count <= ih;
    rows += if free_row { count } else { 0 };
    let rates = free_row && rows + 2 * busy <= ih;
    rows += if rates { 2 * busy } else { 0 };
    let spare = ih.saturating_sub(rows);
    let (grow, mut odd) = match spare.checked_div(busy) {
        Some(grow) if grow < 3 => (grow, spare % busy),
        Some(_) => (3, 0),
        None => (0, 0),
    };
    let (mut y, mut first) = (iy, true);
    each_disk(app, &mut |name, total, used, io| {
        let io_rows = if io.is_some() { 1 + grow + (odd > 0) as u16 } else { 0 };
        odd -= (io.is_some() && odd > 0) as u16;
        let rate_rows = if rates && io.is_some() { 2 } else { 0 };
        if y + 2 + io_rows + rate_rows + free_row as u16 > iy + ih {
            return;
        }
        let total_text = size(total);
        if first {
            put_right(buf, dx + dw, y, &total_text, bold(t.fg));
        } else {
            rule(buf, (edge, last), y, "", &total_text, t, (t.div, t.box_mem));
        }
        first = false;
        let x = put(buf, dx, y, name, bold(t.fg));
        y += 1;
        if let Some(d) = io {
            let graph_w = dw.saturating_sub(4);
            put(buf, dx, y, "IO%", fg(t.fg));
            graph(buf, Rect::new(dx + 4, y, graph_w, io_rows), d.io.data(), 100.0, &t.load, t.inactive, false);
            y += io_rows;
            if rates {
                for (arrow, rate, hist) in [("▲", d.read, &d.read_hist), ("▼", d.write, &d.write_hist)] {
                    put(buf, dx, y, arrow, fg(t.fg));
                    let area = Rect::new(dx + 4, y, graph_w.saturating_sub(11), 1);
                    graph(buf, area, hist.data(), IO_SPEED, &t.available, t.inactive, false);
                    put_right(buf, dx + dw, y, &format!("{}/s", size(rate as u64)), fg(t.fg));
                    y += 1;
                }
            } else {
                // No rows for the rates: they go between name and size.
                let mut speeds = String::new();
                for (arrow, rate) in [("▲", d.read), ("▼", d.write)] {
                    if rate >= 1.0 {
                        speeds.push_str(arrow);
                        speeds.push_str(&human(rate, &SHORT, true));
                    }
                }
                let free = (dx + dw).saturating_sub(x + total_text.chars().count() as u16);
                let w = speeds.chars().count() as u16;
                if w > 0 && w + 2 <= free {
                    put(buf, x + (free - w) / 2, y - io_rows - 1, &speeds, fg(t.fg));
                }
            }
        }
        let meters = [("Used:", used, &t.load), ("Free:", total.saturating_sub(used), &t.free)];
        for (i, (label, part, gradient)) in meters.into_iter().take(1 + free_row as usize).enumerate() {
            let share = pct(part, total);
            let x = put(buf, dx, y, label, fg(t.fg));
            let style = if i == 0 { reading(t, share) } else { fg(t.fg) };
            let x = put(buf, x, y, &format!("{share:>3.0}% "), style);
            gauge(buf, x, y, (dx + dw).saturating_sub(x + 9), share, gradient, t.meter_bg);
            put_right(buf, dx + dw, y, &size(part), fg(t.fg));
            y += 1;
        }
    });
}

fn net_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
    frame(buf, r, t.box_net);
    {
        let iface = format!(" {} ", app.iface);
        let mut bar = Bar::top(buf, r, t.box_net);
        let _ = bar.left(&[("net", bold(t.title))]);
        if !app.ip.is_empty() {
            bar.left(&[(&app.ip, bold(t.title))]);
        }
        if app.ifaces.len() > 1 {
            let arrows = bar.right(&[("←b", fg(t.hi)), (&iface, fg(t.fg)), ("n→", fg(t.hi))]);
            click_ends(hits, arrows, (2, Hit::Key(KeyCode::Char('b'))), (2, Hit::Key(KeyCode::Char('n'))));
        } else if !app.iface.is_empty() {
            bar.right(&[(iface.trim(), fg(t.fg))]);
        }
    }
    let (ix, iy, iw, ih) = inner(r);
    if iw < 20 || ih < 2 {
        return;
    }
    let mut sub_w = iw.min(28);
    if iw < sub_w + 8 {
        sub_w = iw;
    }
    let sub = Rect::new(ix + iw - sub_w, iy, sub_w, ih);
    frame(buf, sub, t.div);
    Bar::top(buf, sub, t.div).left(&[("download", bold(t.title))]);
    Bar::bottom(buf, sub, t.div).left(&[("upload", bold(t.title))]);

    let (x0, sw, sh) = (sub.x + 1, sub_w - 2, ih.saturating_sub(2));
    let lines = (sh / 2).min(3);
    let line = |buf: &mut Buffer, y: u16, arrow: &str, color: Color, left: &str, right: &str| {
        put(buf, x0, y, arrow, fg(color));
        let x = put(buf, x0 + 2, y, left, fg(t.fg));
        if x + 1 + right.chars().count() as u16 <= x0 + sw {
            put_right(buf, x0 + sw, y, right, fg(t.fg));
        }
    };
    let directions = [
        ("▼", t.download.at(100.0), app.rx, app.rx_top, app.rx_total, iy + 1),
        ("▲", t.upload.at(100.0), app.tx, app.tx_top, app.tx_total, iy + 1 + sh - lines),
    ];
    for (arrow, color, now, top, total, y) in directions {
        let bits = |rate: f32| format!("({})", human(rate as f64 * 8.0, &BITS, false));
        let rows = [
            (format!("{}/s", human(now as f64, &BYTES, false)), bits(now)),
            ("Top:".to_string(), bits(top)),
            ("Total:".to_string(), size(total)),
        ];
        for (i, (left, right)) in rows.iter().take(lines as usize).enumerate() {
            line(buf, y + i as u16, arrow, color, left, right);
        }
    }

    // Download grows up and upload hangs below, each on the scale of its
    // own largest rate in view.
    let area = Rect::new(ix, iy, iw - sub_w, ih);
    if area.width < 4 {
        return;
    }
    let scale = |data: &[f32]| {
        let from = data.len().saturating_sub(2 * area.width as usize);
        data[from..].iter().copied().fold(10.0 * 1024.0, f32::max)
    };
    let text = |arrow: &str, scale: f32| {
        (format!("{arrow}{:>4}", human(scale as f64, &SHORT, true)), human(scale as f64 / 2.0, &SHORT, true))
    };
    let (rx, tx) = (app.rx_hist.data(), app.tx_hist.data());
    let (down, up) = (scale(rx), scale(tx));
    let (down_text, up_text) = (text("▼", down), text("▲", up));
    let halves = [(rx, down, &*down_text.0, &*down_text.1, &t.download), (tx, up, &*up_text.0, &*up_text.1, &t.upload)];
    let plot = chart(buf, area, halves, t);
    if plot.x > area.x {
        // The arrows in their graphs' colours, as in the box beside them.
        put(buf, ix, iy, "▼", fg(t.download.at(0.0)));
        put(buf, ix, iy + ih - 1, "▲", fg(t.upload.at(0.0)));
    }
    span(buf, r, plot, t.box_net, app, t);
}

/// A column heading as btop draws it: bold in the title colour, with the
/// shortcut letter (`hot`) highlighted. A click sorts by it.
fn heading(buf: &mut Buffer, hits: &mut Hits, (x, y): (u16, u16), label: &str, hot: Option<usize>, sort: Sort, t: &Theme) {
    put(buf, x, y, label, bold(t.title));
    if let Some(at) = hot {
        put(buf, x + at as u16, y, &label[at..at + 1], bold(t.hi));
    }
    hits.push((Rect::new(x, y, label.len() as u16, 1), Hit::Sort(sort)));
}

type Fact<'a> = (&'static str, Cow<'a, str>, bool);

/// Lays `label value` pairs out along rows `w` wide from (`x0`, `y0`): a
/// pair that does not fit, or is flagged, starts the next row, and a value
/// longer than a row is cut. Draws when given a buffer. Returns the rows
/// taken, `max` at most.
fn flow(mut buf: Option<&mut Buffer>, (x0, y0, w): (u16, u16, u16), max: u16, facts: &[Fact], t: &Theme) -> u16 {
    let (mut x, mut row) = (0, 0);
    for (label, value, starts_row) in facts {
        let need = (label.len() + 1 + value.chars().count()) as u16;
        if x > 0 && (*starts_row || x + need > w) {
            (x, row) = (0, row + 1);
        }
        if row >= max {
            return max;
        }
        if let Some(buf) = buf.as_deref_mut() {
            let at = put(buf, x0 + x, y0 + row, label, bold(t.fg)) + 1;
            putn(buf, at, y0 + row, value, (x0 + w).saturating_sub(at), fg(t.fg));
        }
        x += need + 3;
    }
    row + (x > 0) as u16
}

/// Details of one process, above the list: btop's info pane, plus what the
/// program is and where it lives. Takes the rows it needs from the top of
/// `r` and returns how many.
fn info_pane(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) -> u16 {
    let Some((info, p)) = app.info.as_ref().and_then(|info| Some((info, app.proc(info.pid)?))) else {
        return 0;
    };
    let status = match p.state {
        'R' => "Running",
        'S' => "Sleeping",
        'D' => "Disk wait",
        'Z' => "Zombie",
        'T' => "Stopped",
        't' => "Traced",
        'I' => "Idle",
        _ => "Unknown",
    };
    let parent = app.proc(p.ppid).map_or_else(|| p.ppid.to_string(), |parent| format!("{} {}", parent.pid, parent.name));
    fn add<'a>(facts: &mut Vec<Fact<'a>>, label: &'static str, value: impl Into<Cow<'a, str>>, starts_row: bool) {
        facts.push((label, value.into(), starts_row));
    }
    let f = &mut Vec::with_capacity(24);
    // What it is, where it lives, how it runs, what it holds.
    for (label, value, starts_row) in [
        ("What:", &info.what, true),
        ("Package:", &info.package, false),
        ("Path:", &info.path, true),
        ("Dir:", &info.cwd, false),
        ("Unit:", &info.unit, false),
    ] {
        if let Some(value) = value {
            add(f, label, value, starts_row);
        }
    }
    add(f, "Status:", status, true);
    add(f, "User:", &p.user, false);
    add(f, "Parent:", parent, false);
    add(f, "Nice:", p.nice.to_string(), false);
    add(f, "Started:", &info.started, false);
    add(f, "Elapsed:", clock(app.elapsed(p)), false);
    add(f, "CPU time:", clock(info.cpu_secs), false);
    add(f, "Memory:", size(p.load.mem), true);
    if let Some((peak, swap)) = info.memory {
        add(f, "Peak:", size(peak), false);
        add(f, "Swap:", size(swap), false);
    }
    add(f, "Threads:", p.load.threads.to_string(), false);
    if let Some(files) = info.files {
        add(f, "Files:", files.to_string(), false);
    }
    if let Some((read, written)) = info.io {
        add(f, "Read:", size(read), false);
        add(f, "Written:", size(written), false);
    }
    if p.drm {
        add(f, "GPU:", format!("{:.1}%", p.load.gpu), false);
        add(f, "VRAM:", size(p.vram), false);
    }
    add(f, "Command:", &p.cmd, true);
    let facts: &[Fact] = f;

    // The border and the two meter rows come on top of the facts.
    let at = (r.x + 2, r.y + 1, r.width.saturating_sub(4));
    let rows = flow(None, at, r.height.saturating_sub(4), facts, t);
    let r = Rect { height: rows + 4, ..r };
    frame(buf, r, t.div);
    {
        let pid = format!("{} ", p.pid);
        let mut bar = Bar::top(buf, r, t.div);
        bar.left(&[("info", bold(t.title))]);
        bar.left(&[(&pid, fg(t.hi)), (&p.name, bold(t.fg))]);
        if let Some(area) = bar.right(&[("esc", fg(t.hi)), (" close", fg(t.fg))]) {
            hits.push((area, Hit::Key(KeyCode::Esc)));
        }
    }
    flow(Some(buf), at, rows, facts, t);

    // Meters on the left, the CPU history since the pane opened beside them.
    let (x0, y0, w) = (at.0, at.1 + rows, at.2);
    let all_cores = app.cores.len().max(1) as f32;
    let cpu = p.load.cpu * if app.per_core { all_cores } else { 1.0 };
    let mem = pct(p.load.mem, app.mem.total);
    let half = w / 2;
    let meter_w = half.saturating_sub(4 + 9);
    for (i, (label, part, value)) in
        [("CPU", p.load.cpu * all_cores, format!("{cpu:.1}%")), ("MEM", mem, size(p.load.mem))].into_iter().enumerate()
    {
        let y = y0 + i as u16;
        put(buf, x0, y, label, bold(t.fg));
        meter(buf, x0 + 4, y, meter_w, part, &t.load, t.meter_bg);
        put_right(buf, x0 + half - 1, y, &value, reading(t, part));
    }
    graph(buf, Rect::new(x0 + half + 1, y0, w - half - 1, 2), info.cpu.data(), 100.0, &t.load, t.inactive, false);
    r.height
}

fn proc_box(buf: &mut Buffer, r: Rect, app: &mut App, t: &Theme, hits: &mut Hits) {
    frame(buf, r, t.box_proc);
    app.proc_area = r;
    let (ix, mut iy, iw, mut ih) = inner(r);
    {
        let sort = format!(" {} ", app.sort.label());
        let toggle = |on: bool| if on { bold(t.title) } else { fg(t.fg) };
        let (reverse, per_core, tree) = (toggle(app.reverse), toggle(app.per_core), toggle(app.tree));
        let mut bar = Bar::top(buf, r, t.box_proc);
        let _ = bar.left(&[("proc", bold(t.title))]);
        let filter = if app.filtering || !app.filter.is_empty() {
            let cursor = if app.filtering { "█" } else { "" };
            bar.left(&[("f", fg(t.hi)), (" ", fg(t.fg)), (&app.filter, fg(t.fg)), (cursor, fg(t.hi))])
        } else {
            bar.left(&[("f", fg(t.hi)), ("ilter", fg(t.fg))])
        };
        click(hits, filter, 'f');
        let arrows = bar.right(&[("←", fg(t.hi)), (&sort, fg(t.fg)), ("→", fg(t.hi))]);
        click_ends(hits, arrows, (1, Hit::Key(KeyCode::Left)), (1, Hit::Key(KeyCode::Right)));
        click(hits, bar.right(&[("tr", tree), ("e", fg(t.hi)), ("e", tree)]), 'e');
        click(hits, bar.right(&[("r", fg(t.hi)), ("everse", reverse)]), 'r');
        click(hits, bar.right(&[("per-c", per_core), ("o", fg(t.hi)), ("re", per_core)]), 'o');
    }
    {
        let count = format!("{}/{}", app.selected.map_or(0, |row| row + 1), app.view.len());
        let mut bar = Bar::bottom(buf, r, t.box_proc);
        let arrows = bar.left(&[("↑", fg(t.hi)), (" select ", fg(t.fg)), ("↓", fg(t.hi))]);
        click_ends(hits, arrows, (1, Hit::Key(KeyCode::Up)), (1, Hit::Key(KeyCode::Down)));
        if app.selected.is_some() {
            if let Some(area) = bar.left(&[("info ", fg(t.fg)), ("↵", fg(t.hi))]) {
                hits.push((area, Hit::Key(KeyCode::Enter)));
            }
            click(hits, bar.left(&[("t", fg(t.hi)), ("erminate", fg(t.fg))]), 't');
            click(hits, bar.left(&[("k", fg(t.hi)), ("ill", fg(t.fg))]), 'k');
            click(hits, bar.left(&[("s", fg(t.hi)), ("ignals", fg(t.fg))]), 's');
            let following = if app.follow.is_some() { bold(t.title) } else { fg(t.fg) };
            click(hits, bar.left(&[("F", fg(t.hi)), ("ollow", following)]), 'F');
        }
        bar.right(&[(&count, fg(t.fg))]);
    }
    if iw < 30 || ih < 2 {
        return;
    }
    if app.info.is_some() && ih >= 11 {
        let used = info_pane(buf, Rect::new(ix, iy, iw, ih - 4), app, t, hits);
        iy += used;
        ih -= used;
    }

    // Columns, anchored right to left; narrower boxes drop the command,
    // then threads and the graph, then the user and the GPU share. The
    // numbers stand apart from the text and from each other.
    let right = ix + iw;
    let scroll = right - 1;
    let cpu_x = right - 8;
    let g = if app.gpu_procs { 8 } else { 0 };
    let (graph_x, gpu_x, mem_x, user_x, threads_x) = if iw >= 62 + g {
        let mem_x = cpu_x - 13 - g;
        (Some(cpu_x - 6), (g > 0).then_some(cpu_x - 13), mem_x, Some(mem_x - 12), Some(mem_x - 21))
    } else if iw >= 46 + g {
        let mem_x = cpu_x - 8 - g;
        (None, (g > 0).then_some(cpu_x - 8), mem_x, Some(mem_x - 12), None)
    } else {
        (None, None, cpu_x - 8, None, None)
    };
    let text_end = threads_x.or(user_x).unwrap_or(mem_x) - 1;
    let prog_x = ix + 9;
    // The tree takes the program and command columns together. A wide box
    // shows longer program names.
    let name_w = if iw >= 120 + g { 20 } else { 14 };
    let cmd_x = (iw >= 74 + g && !app.tree).then_some(prog_x + name_w + 1);
    let prog_w = cmd_x.map_or(text_end - prog_x, |_| name_w);

    heading(buf, hits, (ix + 4, iy), "Pid:", None, Sort::Pid, t);
    heading(buf, hits, (prog_x, iy), if app.tree { "Tree:" } else { "Program:" }, None, Sort::Program, t);
    if let Some(x) = cmd_x {
        heading(buf, hits, (x, iy), "Command:", None, Sort::Command, t);
    }
    if let Some(x) = threads_x {
        heading(buf, hits, (x, iy), "Threads:", None, Sort::Threads, t);
    }
    if let Some(x) = user_x {
        heading(buf, hits, (x, iy), "User:", None, Sort::User, t);
    }
    heading(buf, hits, (mem_x + 1, iy), "MemB", Some(0), Sort::Memory, t);
    if let Some(x) = gpu_x {
        heading(buf, hits, (x + 1, iy), "Gpu%", Some(0), Sort::Gpu, t);
    }
    heading(buf, hits, (cpu_x + 1, iy), "Cpu%", Some(0), Sort::Cpu, t);

    // Keep the selection on screen and the list full.
    let rows = (ih - 1) as usize;
    let total = app.view.len();
    app.page_rows = rows;
    if let Some(row) = app.selected {
        app.offset = app.offset.clamp((row + 1).saturating_sub(rows), row);
    }
    app.offset = app.offset.min(total.saturating_sub(rows));

    let all_cores = app.cores.len().max(1) as f32;
    let cores = if app.per_core { all_cores } else { 1.0 };
    let share = |v: f32| if v < 100.0 { format!("{v:.1}") } else { format!("{v:.0}") };
    for (i, row) in app.view.iter().enumerate().skip(app.offset).take(rows) {
        let Some(p) = app.proc(row.pid) else {
            continue;
        };
        let y = iy + 1 + (i - app.offset) as u16;
        hits.push((Rect::new(ix, y, iw - 1, 1), Hit::Row(i)));
        put_right(buf, ix + 8, y, &p.pid.to_string(), fg(t.fg));
        if app.tree {
            // Guides of the ancestors, the branch, a fold mark, the name.
            let room = |x: u16| text_end.saturating_sub(x);
            let mut x = prog_x;
            for level in 1..row.depth {
                let guide = if row.guides >> level & 1 == 1 { " │  " } else { "    " };
                x = putn(buf, x, y, guide, room(x), fg(t.div));
            }
            if row.depth > 0 {
                x = putn(buf, x, y, if row.last { " └─ " } else { " ├─ " }, room(x), fg(t.div));
            }
            if row.kids {
                let mark = x;
                x = putn(buf, x, y, if row.folded { "[+]" } else { "[-]" }, room(x), fg(t.hi));
                hits.push((Rect::new(mark, y, x - mark, 1), Hit::Fold(i)));
                x += 1;
            }
            x = putn(buf, x, y, &p.name, room(x), fg(t.fg)) + 1;
            if p.cmd != p.name {
                putn(buf, x, y, &p.cmd, room(x), fg(t.inactive));
            }
        } else {
            putn(buf, prog_x, y, &p.name, prog_w, fg(t.fg));
            if let Some(x) = cmd_x {
                putn(buf, x, y, &p.cmd, text_end - x, fg(t.inactive));
            }
        }
        if let Some(x) = threads_x {
            put_right(buf, x + 8, y, &row.load.threads.to_string(), fg(t.fg));
        }
        if let Some(x) = user_x {
            putn(buf, x, y, &p.user, 9, fg(t.inactive));
        }
        put_right(buf, mem_x + 5, y, &human(row.load.mem as f64, &SHORT, true), fg(t.fg));
        // Blank for processes that cannot use the GPU at all.
        if let Some(x) = gpu_x.filter(|_| p.drm || row.load.gpu > 0.0) {
            let color = if row.load.gpu < 0.05 { t.inactive } else { t.load.at(row.load.gpu) };
            put_right(buf, x + 5, y, &share(row.load.gpu), fg(color));
        }
        if let Some(x) = graph_x {
            graph(buf, Rect::new(x, y, 5, 1), p.history(), 100.0, &t.load, t.inactive, false);
        }
        let cpu = row.load.cpu * cores;
        let color = if cpu < 0.05 { t.inactive } else { t.load.at(row.load.cpu * all_cores) };
        put_right(buf, cpu_x + 5, y, &share(cpu), fg(color));
        if app.selected == Some(i) {
            let style = Style::new().fg(t.selected_fg).bg(t.selected_bg).add_modifier(Modifier::BOLD);
            buf.set_style(Rect::new(ix, y, iw - 1, 1), style);
        }
    }

    put(buf, scroll, iy, "↑", fg(t.hi));
    put(buf, scroll, iy + ih - 1, "↓", fg(t.hi));
    let track = ih.saturating_sub(2) as usize;
    if total > rows && track > 0 {
        let at = app.offset * (track - 1) / (total - rows);
        put(buf, scroll, iy + 1 + at as u16, "█", fg(t.fg));
    }
}

fn dialog_box(buf: &mut Buffer, a: Rect, d: &Dialog, t: &Theme, hits: &mut Hits) {
    // A cleared, framed box in the middle of the screen.
    let open = |buf: &mut Buffer, w: u16, h: u16, title: &str| {
        let r = Rect::new(a.width.saturating_sub(w) / 2, a.height.saturating_sub(h) / 2, w, h);
        for y in r.top()..r.bottom().min(a.bottom()) {
            for x in r.left()..r.right().min(a.right()) {
                buf[(x, y)].reset();
            }
        }
        frame(buf, r, t.hi);
        Bar::top(buf, r, t.hi).left(&[(title, bold(t.title))]);
        r
    };
    match d.ask {
        Ask::Confirm(signal) => {
            let verb = if signal == libc::SIGKILL { "kill" } else { "terminate" };
            let name = SIGNALS.iter().find(|s| s.0 == signal).map_or("the signal", |s| s.1);
            let question = format!("Send {name} to {} ({})?", d.pid, d.name);
            let w = (question.chars().count() as u16 + 4).clamp(34, a.width - 4);
            let r = open(buf, w, 6, verb);
            let (x, y) = (r.x + 2, r.y + 3);
            putn(buf, x, r.y + 2, &question, w - 4, fg(t.fg));
            if let Some(error) = &d.error {
                putn(buf, x, y, error, w - 4, bold(t.hi));
                return;
            }
            let yes = put(buf, x, y, "y", bold(t.hi));
            let no = put(buf, yes, y, "es", fg(t.fg)) + 4;
            let n = put(buf, no, y, "n", bold(t.hi));
            put(buf, n, y, "o", fg(t.fg));
            hits.push((Rect::new(x, y, 3, 1), Hit::Key(KeyCode::Char('y'))));
            hits.push((Rect::new(no, y, 2, 1), Hit::Key(KeyCode::Char('n'))));
        }
        Ask::Pick(cursor) => {
            const CELL: u16 = 13;
            let rows = SIGNAL_ROWS as u16;
            let cols = (SIGNALS.len() as u16).div_ceil(rows);
            let w = cols * CELL + 3;
            let r = open(buf, w, rows + 7, "signals");
            let x = r.x + 2;
            putn(buf, x, r.y + 2, &format!("Send to {} ({}):", d.pid, d.name), w - 4, fg(t.fg));
            for (i, (number, name)) in SIGNALS.iter().enumerate() {
                let i16 = i as u16;
                let cell = Rect::new(x + i16 / rows * CELL, r.y + 4 + i16 % rows, CELL - 1, 1);
                let end = put(buf, cell.x, cell.y, &format!("{number:>2} "), fg(t.hi));
                put(buf, end, cell.y, name, fg(t.fg));
                if i == cursor {
                    buf.set_style(cell, Style::new().fg(t.selected_fg).bg(t.selected_bg).add_modifier(Modifier::BOLD));
                }
                hits.push((cell, Hit::Signal(i)));
            }
            let y = r.bottom() - 2;
            if let Some(error) = &d.error {
                putn(buf, x, y, error, w - 4, bold(t.hi));
                return;
            }
            let mut at = x;
            for (key, what) in [("↑↓←→", " choose   "), ("enter", " send   "), ("esc", " cancel")] {
                at = put(buf, at, y, key, fg(t.hi));
                at = put(buf, at, y, what, fg(t.fg));
            }
        }
    }
}
