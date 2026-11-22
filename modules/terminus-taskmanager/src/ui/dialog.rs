//! The modal dialogs of the process box: confirm a signal, or pick one.

use crate::app::{Ask, Dialog, Hit, Hits, SIGNAL_ROWS};
use crate::draw::{bold, fg, frame, put, putn, Bar};
use crate::sys::SIGNALS;
use crate::theme::Theme;
use ratatui::{
    buffer::Buffer,
    crossterm::event::KeyCode,
    layout::Rect,
    style::{Modifier, Style},
};

/// A cleared, framed box in the middle of the screen `a`.
fn open_dialog(buf: &mut Buffer, a: Rect, (w, h): (u16, u16), title: &str, t: &Theme) -> Rect {
    let r = Rect::new(a.width.saturating_sub(w) / 2, a.height.saturating_sub(h) / 2, w, h);
    for y in r.top()..r.bottom().min(a.bottom()) {
        for x in r.left()..r.right().min(a.right()) {
            buf[(x, y)].reset();
        }
    }
    frame(buf, r, t.hi);
    Bar::top(buf, r, t.hi).left(&[(title, bold(t.title))]);
    r
}

/// Shows a failed send in place of the dialog's keys; false when there is none.
fn dialog_error(buf: &mut Buffer, d: &Dialog, (x, y, w): (u16, u16, u16), t: &Theme) -> bool {
    let Some(error) = &d.error else {
        return false;
    };
    putn(buf, x, y, error, w, bold(t.hi));
    true
}

fn confirm_dialog(buf: &mut Buffer, a: Rect, d: &Dialog, signal: i32, t: &Theme, hits: &mut Hits) {
    let verb = if signal == libc::SIGKILL { "kill" } else { "terminate" };
    let name = SIGNALS.iter().find(|s| s.0 == signal).map_or("the signal", |s| s.1);
    let question = format!("Send {name} to {} ({})?", d.pid, d.name);
    let w = (question.chars().count() as u16 + 4).clamp(34, a.width - 4);
    let r = open_dialog(buf, a, (w, 6), verb, t);
    let (x, y) = (r.x + 2, r.y + 3);
    putn(buf, x, r.y + 2, &question, w - 4, fg(t.fg));
    if dialog_error(buf, d, (x, y, w - 4), t) {
        return;
    }
    let yes = put(buf, x, y, "y", bold(t.hi));
    let no = put(buf, yes, y, "es", fg(t.fg)) + 4;
    let n = put(buf, no, y, "n", bold(t.hi));
    put(buf, n, y, "o", fg(t.fg));
    hits.push((Rect::new(x, y, 3, 1), Hit::Key(KeyCode::Char('y'))));
    hits.push((Rect::new(no, y, 2, 1), Hit::Key(KeyCode::Char('n'))));
}

fn pick_dialog(buf: &mut Buffer, a: Rect, d: &Dialog, cursor: usize, t: &Theme, hits: &mut Hits) {
    const CELL_W: u16 = 13;
    let rows = SIGNAL_ROWS as u16;
    let cols = (SIGNALS.len() as u16).div_ceil(rows);
    let w = cols * CELL_W + 3;
    let r = open_dialog(buf, a, (w, rows + 7), "signals", t);
    let x = r.x + 2;
    putn(buf, x, r.y + 2, &format!("Send to {} ({}):", d.pid, d.name), w - 4, fg(t.fg));
    for (i, (number, name)) in SIGNALS.iter().enumerate() {
        let slot = i as u16;
        let cell = Rect::new(x + slot / rows * CELL_W, r.y + 4 + slot % rows, CELL_W - 1, 1);
        let end = put(buf, cell.x, cell.y, &format!("{number:>2} "), fg(t.hi));
        put(buf, end, cell.y, name, fg(t.fg));
        if i == cursor {
            buf.set_style(cell, Style::new().fg(t.selected_fg).bg(t.selected_bg).add_modifier(Modifier::BOLD));
        }
        hits.push((cell, Hit::Signal(i)));
    }
    let y = r.bottom() - 2;
    if dialog_error(buf, d, (x, y, w - 4), t) {
        return;
    }
    let mut at = x;
    for (key, what) in [("↑↓←→", " choose   "), ("enter", " send   "), ("esc", " cancel")] {
        at = put(buf, at, y, key, fg(t.hi));
        at = put(buf, at, y, what, fg(t.fg));
    }
}

pub(super) fn dialog_box(buf: &mut Buffer, a: Rect, d: &Dialog, t: &Theme, hits: &mut Hits) {
    match d.ask {
        Ask::Confirm(signal) => confirm_dialog(buf, a, d, signal, t, hits),
        Ask::Pick(cursor) => pick_dialog(buf, a, d, cursor, t, hits),
    }
}
