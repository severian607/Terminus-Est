//! The net box: traffic totals beside the download and upload graphs.

use super::common::{chart, click_ends, human, size, span, BITS, BYTES, SHORT};
use crate::app::{App, Hit, Hits};
use crate::draw::{bold, fg, frame, inner, put, put_right, Bar};
use crate::theme::Theme;
use ratatui::{buffer::Buffer, crossterm::event::KeyCode, layout::Rect, style::Color};

/// The net box's title row: the address and the interface, which `b` and
/// `n` step through when there are several.
fn net_title_bar(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
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

/// The net info box `sub`: the download's rate, top and total under the
/// download title at the top, and the upload's above the upload title at
/// the bottom, as many lines of each as fit.
fn net_totals(buf: &mut Buffer, sub: Rect, app: &App, t: &Theme) {
    frame(buf, sub, t.div);
    Bar::top(buf, sub, t.div).left(&[("download", bold(t.title))]);
    Bar::bottom(buf, sub, t.div).left(&[("upload", bold(t.title))]);

    let (x0, inner_w, inner_h) = (sub.x + 1, sub.width - 2, sub.height.saturating_sub(2));
    let lines = (inner_h / 2).min(3);
    let line = |buf: &mut Buffer, y: u16, arrow: &str, color: Color, left: &str, right: &str| {
        put(buf, x0, y, arrow, fg(color));
        let x = put(buf, x0 + 2, y, left, fg(t.fg));
        if x + 1 + right.chars().count() as u16 <= x0 + inner_w {
            put_right(buf, x0 + inner_w, y, right, fg(t.fg));
        }
    };
    let directions = [
        ("▼", t.download.at(100.0), app.rx, app.rx_top, app.rx_total, sub.y + 1),
        ("▲", t.upload.at(100.0), app.tx, app.tx_top, app.tx_total, sub.y + 1 + inner_h - lines),
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
}

/// The traffic graphs in `area`: download grows up and upload hangs below,
/// each on the scale of its own largest rate in view. Returns where the
/// graphs went.
fn net_chart(buf: &mut Buffer, area: Rect, app: &App, t: &Theme) -> Rect {
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
        put(buf, area.x, area.y, "▼", fg(t.download.at(0.0)));
        put(buf, area.x, area.bottom() - 1, "▲", fg(t.upload.at(0.0)));
    }
    plot
}

pub(super) fn net_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
    frame(buf, r, t.box_net);
    net_title_bar(buf, r, app, t, hits);
    let (ix, iy, iw, ih) = inner(r);
    if iw < 20 || ih < 2 {
        return;
    }
    let mut sub_w = iw.min(28);
    if iw < sub_w + 8 {
        sub_w = iw;
    }
    net_totals(buf, Rect::new(ix + iw - sub_w, iy, sub_w, ih), app, t);
    let area = Rect::new(ix, iy, iw - sub_w, ih);
    if area.width < 4 {
        return;
    }
    let plot = net_chart(buf, area, app, t);
    span(buf, r, plot, t.box_net, app, t);
}
