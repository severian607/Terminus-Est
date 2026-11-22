//! The mem box: memory on the left, the disks and swap on the right.

use super::common::{human, reading, rule, size, SHORT};
use crate::app::{pct, App, Disk};
use crate::draw::{bold, fg, frame, gauge, graph, inner, meter, put, put_right, vline, Bar};
use crate::theme::Theme;
use ratatui::{buffer::Buffer, layout::Rect};

/// What btop draws at the top of a disk's read and write graphs.
const IO_SPEED: f32 = 10.0 * 1024.0 * 1024.0;

/// A disk, or swap, as the mem box lists it.
struct DiskRow<'a> {
    name: &'a str,
    total: u64,
    used: u64,
    /// The disk itself when it has I/O statistics.
    io: Option<&'a Disk>,
}

/// The disks in the order shown: those with a size, and swap as the second,
/// as btop lists it.
fn disk_rows(app: &App) -> Vec<DiskRow<'_>> {
    let mut rows: Vec<DiskRow> = app
        .disks
        .iter()
        .filter(|d| d.total > 0)
        .map(|d| DiskRow { name: &d.name, total: d.total, used: d.used, io: (!d.io.data().is_empty()).then_some(d) })
        .collect();
    let m = &app.mem;
    if m.swap_total > 0 {
        let swap = DiskRow { name: "swap", total: m.swap_total, used: m.swap_total - m.swap_free, io: None };
        rows.insert(rows.len().min(1), swap);
    }
    rows
}

/// The memory box is as tall as its disks ask for with a used and a free
/// meter each, within what leaves the net box its three lines per
/// direction and at most 55% of the `height` the two share.
pub(super) fn mem_height(app: &App, height: u16, with_net: bool) -> u16 {
    if !with_net {
        return height;
    }
    if height < 21 {
        return height * 58 / 100;
    }
    let disks = disk_rows(app);
    let busy = disks.iter().filter(|d| d.io.is_some()).count() as u16;
    let net_max = (height * 55 / 100).max(10);
    (3 * disks.len() as u16 + busy + 2).clamp(height - net_max, height - 10)
}

/// The left half of the mem box: the total, then used, available, cached
/// and free memory, each a graph under a ruled label when rows allow, else
/// one row with a meter. `mem_w` is the half's width inside the border;
/// `split` says a divider closes it.
fn memory_column(buf: &mut Buffer, r: Rect, (mem_w, split): (u16, bool), app: &App, t: &Theme) {
    let (ix, iy, _, ih) = inner(r);
    let m = &app.mem;
    // Column that closes the half: the divider, or the box's border.
    let edge = ix + mem_w;
    let edge_color = if split { t.div } else { t.box_mem };
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
            let rows = per_item - 1 + u16::from((i as u16) < extra);
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
}

/// How the rows of the disks column are spent: spare rows first give every
/// disk a free-space meter, as btop does, then graphs of what it reads and
/// writes, then taller busy graphs.
struct DiskPlan {
    free_row: bool,
    rates: bool,
    /// Rows added to every busy graph, and how many busy disks get one more.
    grow: u16,
    odd: u16,
}

impl DiskPlan {
    /// The most a busy graph grows by.
    const MAX_GROWTH: u16 = 3;

    fn new(disks: &[DiskRow], height: u16) -> DiskPlan {
        let count = disks.len() as u16;
        let busy = disks.iter().filter(|d| d.io.is_some()).count() as u16;
        let mut rows = 2 * count + busy;
        let free_row = rows + count <= height;
        rows += if free_row { count } else { 0 };
        let rates = free_row && rows + 2 * busy <= height;
        rows += if rates { 2 * busy } else { 0 };
        let spare = height.saturating_sub(rows);
        let (grow, odd) = match spare.checked_div(busy) {
            Some(grow) if grow < DiskPlan::MAX_GROWTH => (grow, spare % busy),
            Some(_) => (DiskPlan::MAX_GROWTH, 0),
            None => (0, 0),
        };
        DiskPlan { free_row, rates, grow, odd }
    }

    /// Rows of the busy graph of the next disk: one, what every graph
    /// grows by, and one more while odd rows are left.
    fn take_io_rows(&mut self, busy: bool) -> u16 {
        if !busy {
            return 0;
        }
        let extra = self.odd > 0;
        self.odd -= u16::from(extra);
        1 + self.grow + u16::from(extra)
    }
}

/// The right half of the mem box: a block per disk, with its name and size,
/// how busy it is and how full it is.
struct DiskColumn<'t> {
    /// Left edge and width of the text.
    x: u16,
    width: u16,
    plan: DiskPlan,
    theme: &'t Theme,
}

impl DiskColumn<'_> {
    /// Draws `disk` from row `y`, its busy graph `io_rows` tall. Returns the
    /// row after it.
    fn draw(&self, buf: &mut Buffer, mut y: u16, first: bool, disk: &DiskRow, io_rows: u16) -> u16 {
        let (dx, dw, t) = (self.x, self.width, self.theme);
        let total_text = size(disk.total);
        if first {
            put_right(buf, dx + dw, y, &total_text, bold(t.fg));
        } else {
            // From the divider to the box's border.
            rule(buf, (dx - 2, dx + dw + 1), y, "", &total_text, t, (t.div, t.box_mem));
        }
        let name_end = put(buf, dx, y, disk.name, bold(t.fg));
        let name_y = y;
        y += 1;
        if let Some(d) = disk.io {
            y = self.draw_io(buf, y, d, io_rows);
            if !self.plan.rates {
                // No rows for the rates: they go between name and size.
                let free = (dx + dw).saturating_sub(name_end + total_text.chars().count() as u16);
                let mut speeds = String::new();
                for (arrow, rate) in [("▲", d.read), ("▼", d.write)] {
                    if rate >= 1.0 {
                        speeds.push_str(arrow);
                        speeds.push_str(&human(rate, &SHORT, true));
                    }
                }
                let w = speeds.chars().count() as u16;
                if w > 0 && w + 2 <= free {
                    put(buf, name_end + (free - w) / 2, name_y, &speeds, fg(t.fg));
                }
            }
        }
        let meters = [("Used:", disk.used, &t.load), ("Free:", disk.total.saturating_sub(disk.used), &t.free)];
        for (i, (label, part, gradient)) in meters.into_iter().take(1 + usize::from(self.plan.free_row)).enumerate() {
            let share = pct(part, disk.total);
            let x = put(buf, dx, y, label, fg(t.fg));
            let style = if i == 0 { reading(t, share) } else { fg(t.fg) };
            let x = put(buf, x, y, &format!("{share:>3.0}% "), style);
            gauge(buf, x, y, (dx + dw).saturating_sub(x + 9), share, gradient, t.meter_bg);
            put_right(buf, dx + dw, y, &size(part), fg(t.fg));
            y += 1;
        }
        y
    }

    /// The busy graph of `d` from row `y`, and below it, with room, the
    /// graphs of what it reads and writes. Returns the row after them.
    fn draw_io(&self, buf: &mut Buffer, mut y: u16, d: &Disk, io_rows: u16) -> u16 {
        let (dx, dw, t) = (self.x, self.width, self.theme);
        let graph_w = dw.saturating_sub(4);
        put(buf, dx, y, "IO%", fg(t.fg));
        graph(buf, Rect::new(dx + 4, y, graph_w, io_rows), d.io.data(), 100.0, &t.load, t.inactive, false);
        y += io_rows;
        if self.plan.rates {
            for (arrow, rate, hist) in [("▲", d.read, &d.read_hist), ("▼", d.write, &d.write_hist)] {
                put(buf, dx, y, arrow, fg(t.fg));
                let area = Rect::new(dx + 4, y, graph_w.saturating_sub(11), 1);
                graph(buf, area, hist.data(), IO_SPEED, &t.available, t.inactive, false);
                put_right(buf, dx + dw, y, &format!("{}/s", size(rate as u64)), fg(t.fg));
                y += 1;
            }
        }
        y
    }
}

/// The right half of the mem box `r`, from column `mem_w` inside it on.
fn disks_column(buf: &mut Buffer, r: Rect, mem_w: u16, app: &App, t: &Theme) {
    let (ix, iy, iw, ih) = inner(r);
    let edge = ix + mem_w;
    Bar::new(buf, r.y, edge + 1, r.right() - 1, t.box_mem, false).left(&[("disks", bold(t.title))]);
    let disks = disk_rows(app);
    let mut column = DiskColumn { x: edge + 2, width: iw - mem_w - 3, plan: DiskPlan::new(&disks, ih), theme: t };
    let (mut y, mut first) = (iy, true);
    for disk in &disks {
        let io_rows = column.plan.take_io_rows(disk.io.is_some());
        let rate_rows = if column.plan.rates && disk.io.is_some() { 2 } else { 0 };
        // A disk that does not fit whole is left out; a smaller one may.
        if y + 2 + io_rows + rate_rows + u16::from(column.plan.free_row) > iy + ih {
            continue;
        }
        y = column.draw(buf, y, first, disk, io_rows);
        first = false;
    }
}

pub(super) fn mem_box(buf: &mut Buffer, r: Rect, app: &App, t: &Theme) {
    frame(buf, r, t.box_mem);
    let _ = Bar::top(buf, r, t.box_mem).left(&[("mem", bold(t.title))]);
    let (ix, iy, iw, ih) = inner(r);
    if iw < 20 || ih < 2 {
        return;
    }
    let split = iw >= 44;
    let mem_w = if split { iw / 2 } else { iw };
    if split {
        put(buf, ix + mem_w, r.y, "┬", fg(t.box_mem));
        vline(buf, ix + mem_w, iy, ih, t.div);
        put(buf, ix + mem_w, r.bottom() - 1, "┴", fg(t.box_mem));
    }
    memory_column(buf, r, (mem_w, split), app, t);
    if split {
        disks_column(buf, r, mem_w, app, t);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(app: &App) -> Vec<&str> {
        disk_rows(app).iter().map(|d| d.name).collect()
    }

    #[test]
    fn swap_is_listed_second() {
        let app = App::fixture();
        assert_eq!(names(&app), ["root", "swap", "data", "boot"]);
    }

    #[test]
    fn swap_is_listed_even_when_the_other_disk_has_no_size() {
        // A mount statvfs cannot read counts as a disk of size 0 and is
        // not shown; swap still takes the place after the one that is.
        let mut app = App::fixture();
        app.disks.truncate(2);
        app.disks[1].total = 0;
        assert_eq!(names(&app), ["root", "swap"]);
    }
}
