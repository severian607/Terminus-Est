//! What every box shares: sizes and times as text, clickable titles, and the mirrored chart with its rules.

use crate::app::{App, Hit, Hits};
use crate::draw::{bold, fg, graph, half_line, hline, put, put_right, Bar};
use crate::theme::{Gradient, Theme};
use ratatui::{
    buffer::Buffer,
    crossterm::event::KeyCode,
    layout::Rect,
    style::{Color, Style},
};

pub(super) const BYTES: [&str; 5] = ["Byte", "KiB", "MiB", "GiB", "TiB"];
pub(super) const SHORT: [&str; 5] = ["B", "K", "M", "G", "T"];
pub(super) const BITS: [&str; 5] = ["bps", "Kibps", "Mibps", "Gibps", "Tibps"];

/// Three significant digits and a binary unit, as btop prints sizes.
pub(super) fn human(value: f64, units: &[&str; 5], short: bool) -> String {
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

/// `15.4 GiB`: bytes in full words.
pub(super) fn size(bytes: u64) -> String {
    human(bytes as f64, &BYTES, false)
}

/// `04:12:33`, with the days in front once there are any.
pub(super) fn clock(secs: u64) -> String {
    let (d, h, m, s) = (secs / 86400, secs / 3600 % 24, secs / 60 % 60, secs % 60);
    if d > 0 {
        format!("{d}d {h:02}:{m:02}:{s:02}")
    } else {
        format!("{h:02}:{m:02}:{s:02}")
    }
}

/// The first `max` characters of `s`.
pub(super) fn truncated(s: &str, max: u16) -> &str {
    s.char_indices().nth(max as usize).map_or(s, |(i, _)| &s[..i])
}

/// Makes a title clickable: a click presses `key`.
pub(super) fn click(hits: &mut Hits, title: Option<Rect>, key: char) {
    if let Some(area) = title {
        hits.push((area, Hit::Key(KeyCode::Char(key))));
    }
}

/// Makes the first and last cells of a title clickable, each `(width, hit)`.
pub(super) fn click_ends(hits: &mut Hits, title: Option<Rect>, first: (u16, Hit), last: (u16, Hit)) {
    if let Some(area) = title {
        hits.push((Rect { width: first.0, ..area }, first.1));
        hits.push((Rect { x: area.right() - last.0, width: last.0, ..area }, last.1));
    }
}

/// A reading in the text colour, or bold in its alarm colour once it is high.
pub(super) fn reading(t: &Theme, pct: f32) -> Style {
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
pub(super) type Half<'a> = (&'a [f32], f32, &'a str, &'a str, &'a Gradient);

/// A mirrored pair of graphs filling `r`, as btop draws them: the first
/// grows up from the middle and the second hangs below it. With room, the
/// scale stands down the left: each half's full value at its outer edge,
/// `0` at the middle, and its half value on a dotted line. Returns where
/// the graphs went.
pub(super) fn chart(buf: &mut Buffer, r: Rect, halves: [Half; 2], t: &Theme) -> Rect {
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
pub(super) fn span<'b>(buf: &'b mut Buffer, r: Rect, graph: Rect, line: Color, app: &App, t: &Theme) -> Bar<'b> {
    let mut bar = Bar::new(buf, r.bottom() - 1, graph.x.max(r.x + 2) - 1, r.right() - 1, line, true);
    if graph.width >= 4 {
        bar.left(&[(&ago(app, graph.width), fg(t.graph_text))]);
    }
    bar
}

/// `├─label──────value─┤` across columns `left..=right`, the way btop
/// separates items inside a box. The line shows through the value's space.
pub(super) fn rule(
    buf: &mut Buffer,
    (left, right): (u16, u16),
    y: u16,
    label: &str,
    value: &str,
    t: &Theme,
    ends: (Color, Color),
) {
    put(buf, left, y, "├", fg(ends.0));
    hline(buf, left + 1, y, right.saturating_sub(left + 1), t.div);
    put(buf, right, y, "┤", fg(ends.1));
    put(buf, left + 2, y, label, fg(t.fg));
    let mut x = right.saturating_sub(1 + value.chars().count() as u16);
    for word in value.split(' ') {
        x = put(buf, x, y, word, fg(t.fg)) + 1;
    }
}
