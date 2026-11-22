//! The boxes, laid out as btop does it except that the GPU sits left of the
//! CPU instead of under it:
//!
//!   gpu0 | cpu
//!   mem  | proc
//!   net  |

mod common;
mod devices;
mod dialog;
mod mem;
mod net;
mod procs;

use crate::app::{App, Hits};
use crate::draw::{fg, put};
use crate::theme::Theme;
use devices::{cpu_box, gpu_box};
use dialog::dialog_box;
use mem::{mem_box, mem_height};
use net::net_box;
use procs::proc_box;
use ratatui::{buffer::Buffer, layout::Rect, Frame};

/// Smallest terminal the boxes fit in.
const MIN_SIZE: (u16, u16) = (80, 24);
/// Width of the gpu and the left column, in percent of the screen.
const LEFT_PERCENT: u16 = 45;

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

/// Places the boxes that are shown and draws them, then the dialog if one is open.
fn layout(buf: &mut Buffer, a: Rect, app: &mut App, t: &Theme, hits: &mut Hits) {
    let note = |buf: &mut Buffer, msg: &str| {
        put(buf, a.width.saturating_sub(msg.len() as u16) / 2, a.height / 2, msg, fg(t.fg));
    };
    if a.width < MIN_SIZE.0 || a.height < MIN_SIZE.1 {
        return note(buf, &format!("Terminal is {}x{}, needs {}x{}", a.width, a.height, MIN_SIZE.0, MIN_SIZE.1));
    }
    let s = &app.shown;
    let (top, left) = (s.cpu || s.gpu, s.mem || s.net);
    let bottom = left || s.proc;
    if !top && !bottom {
        return note(buf, "Every box is hidden: 1 cpu  2 mem  3 net  4 proc  5 gpu");
    }
    let left_w = a.width * LEFT_PERCENT / 100;
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
    let mem_h = mem_height(app, side.height, net);
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

#[cfg(test)]
mod tests;
