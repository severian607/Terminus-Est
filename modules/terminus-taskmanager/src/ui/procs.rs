//! The process box: its columns and rows, the info pane, and the headings that sort.

use super::common::{click, click_ends, clock, human, reading, size, SHORT};
use crate::app::{pct, App, Hit, Hits, Info, Proc, Row, Sort};
use crate::draw::{bold, fg, frame, graph, inner, meter, put, put_right, putn, Bar};
use crate::theme::Theme;
use ratatui::{
    buffer::Buffer,
    crossterm::event::KeyCode,
    layout::Rect,
    style::{Modifier, Style},
};
use std::borrow::Cow;

/// A column heading as btop draws it: bold in the title colour, with the
/// first letter highlighted as the shortcut when `shortcut`. A click sorts
/// by it.
fn heading(buf: &mut Buffer, hits: &mut Hits, (x, y): (u16, u16), label: &str, shortcut: bool, sort: Sort, t: &Theme) {
    put(buf, x, y, label, bold(t.title));
    if shortcut {
        put(buf, x, y, &label[..1], bold(t.hi));
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
    row + u16::from(x > 0)
}

/// The facts of the info pane in reading order: what the process is, where
/// it lives, how it runs, what it holds.
fn info_facts<'a>(app: &'a App, info: &'a Info, p: &'a Proc) -> Vec<Fact<'a>> {
    fn add<'a>(facts: &mut Vec<Fact<'a>>, label: &'static str, value: impl Into<Cow<'a, str>>, starts_row: bool) {
        facts.push((label, value.into(), starts_row));
    }
    let parent =
        app.proc(p.ppid).map_or_else(|| p.ppid.to_string(), |parent| format!("{} {}", parent.pid, parent.name));
    let mut f = Vec::with_capacity(24);
    for (label, value, starts_row) in [
        ("What:", &info.what, true),
        ("Package:", &info.package, false),
        ("Path:", &info.path, true),
        ("Dir:", &info.cwd, false),
        ("Unit:", &info.unit, false),
    ] {
        if let Some(value) = value {
            add(&mut f, label, value, starts_row);
        }
    }
    add(&mut f, "Status:", p.state.label(), true);
    add(&mut f, "User:", &p.user, false);
    add(&mut f, "Parent:", parent, false);
    add(&mut f, "Nice:", p.nice.to_string(), false);
    add(&mut f, "Started:", &info.started, false);
    add(&mut f, "Elapsed:", clock(app.elapsed(p)), false);
    add(&mut f, "CPU time:", clock(info.cpu_secs), false);
    add(&mut f, "Memory:", size(p.load.mem), true);
    if let Some((peak, swap)) = info.memory {
        add(&mut f, "Peak:", size(peak), false);
        add(&mut f, "Swap:", size(swap), false);
    }
    add(&mut f, "Threads:", p.load.threads.to_string(), false);
    if let Some(files) = info.files {
        add(&mut f, "Files:", files.to_string(), false);
    }
    if let Some((read, written)) = info.io {
        add(&mut f, "Read:", size(read), false);
        add(&mut f, "Written:", size(written), false);
    }
    if p.drm {
        add(&mut f, "GPU:", format!("{:.1}%", p.load.gpu), false);
        add(&mut f, "VRAM:", size(p.vram), false);
    }
    add(&mut f, "Command:", &p.cmd, true);
    f
}

/// Meters on the left of `area` (x, y, width), two rows, and the CPU
/// history since the pane opened beside them.
fn info_meters(buf: &mut Buffer, area: (u16, u16, u16), app: &App, info: &Info, p: &Proc, t: &Theme) {
    let (x0, y0, w) = area;
    let all_cores = app.cores.len().max(1) as f32;
    let cpu = p.load.cpu * if app.per_core { all_cores } else { 1.0 };
    let mem = pct(p.load.mem, app.mem.total);
    let half = w / 2;
    let meter_w = half.saturating_sub(4 + 9);
    let meters = [("CPU", p.load.cpu * all_cores, format!("{cpu:.1}%")), ("MEM", mem, size(p.load.mem))];
    for (i, (label, part, value)) in meters.into_iter().enumerate() {
        let y = y0 + i as u16;
        put(buf, x0, y, label, bold(t.fg));
        meter(buf, x0 + 4, y, meter_w, part, &t.load, t.meter_bg);
        put_right(buf, x0 + half - 1, y, &value, reading(t, part));
    }
    graph(buf, Rect::new(x0 + half + 1, y0, w - half - 1, 2), info.cpu.data(), 100.0, &t.load, t.inactive, false);
}

/// Details of one process, above the list: btop's info pane, plus what the
/// program is and where it lives. Takes the rows it needs from the top of
/// `r` and returns how many.
fn info_pane(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) -> u16 {
    let Some((info, p)) = app.info.as_ref().and_then(|info| Some((info, app.proc(info.pid)?))) else {
        return 0;
    };
    let facts = info_facts(app, info, p);

    // The border and the two meter rows come on top of the facts.
    let at = (r.x + 2, r.y + 1, r.width.saturating_sub(4));
    let rows = flow(None, at, r.height.saturating_sub(4), &facts, t);
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
    flow(Some(buf), at, rows, &facts, t);
    info_meters(buf, (at.0, at.1 + rows, at.2), app, info, p, t);
    r.height
}

/// The title row of the process box: filter, sort and the view toggles.
fn proc_top_bar(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
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

/// The bottom row of the process box: the keys that act on the selection,
/// and where the selection is.
fn proc_bottom_bar(buf: &mut Buffer, r: Rect, app: &App, t: &Theme, hits: &mut Hits) {
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

/// Width of the Gpu% column, which exists while some process holds a DRM client.
const GPU_COLUMN_W: u16 = 8;

/// Where each column of the process list starts. They are anchored right to
/// left; narrower boxes drop the command, then threads and the graph, then
/// the user and the GPU share. The numbers stand apart from the text and
/// from each other.
struct Columns {
    /// Right edge of the pid, which is right-aligned.
    pid_end: u16,
    prog_x: u16,
    /// Width of the program name while the command column follows it.
    prog_w: u16,
    cmd_x: Option<u16>,
    threads_x: Option<u16>,
    user_x: Option<u16>,
    mem_x: u16,
    gpu_x: Option<u16>,
    graph_x: Option<u16>,
    cpu_x: u16,
    /// Where the text columns must stop: the cell before the first number.
    text_end: u16,
    scroll_x: u16,
}

impl Columns {
    fn new(ix: u16, iw: u16, with_gpu: bool, tree: bool) -> Columns {
        let scroll_x = ix + iw - 1;
        let cpu_x = ix + iw - 8;
        let gpu_w = if with_gpu { GPU_COLUMN_W } else { 0 };
        let (graph_x, gpu_x, mem_x, user_x, threads_x) = if iw >= 62 + gpu_w {
            let mem_x = cpu_x - 13 - gpu_w;
            (Some(cpu_x - 6), with_gpu.then_some(cpu_x - 13), mem_x, Some(mem_x - 12), Some(mem_x - 21))
        } else if iw >= 46 + gpu_w {
            let mem_x = cpu_x - 8 - gpu_w;
            (None, with_gpu.then_some(cpu_x - 8), mem_x, Some(mem_x - 12), None)
        } else {
            (None, None, cpu_x - 8, None, None)
        };
        let text_end = threads_x.or(user_x).unwrap_or(mem_x) - 1;
        let prog_x = ix + 9;
        // The tree takes the program and command columns together. A wide
        // box shows longer program names.
        let name_w = if iw >= 120 + gpu_w { 20 } else { 14 };
        let cmd_x = (iw >= 74 + gpu_w && !tree).then_some(prog_x + name_w + 1);
        let prog_w = cmd_x.map_or(text_end - prog_x, |_| name_w);
        Columns {
            pid_end: ix + 8,
            prog_x,
            prog_w,
            cmd_x,
            threads_x,
            user_x,
            mem_x,
            gpu_x,
            graph_x,
            cpu_x,
            text_end,
            scroll_x,
        }
    }

    /// The heading row; a click on a heading sorts by it.
    fn draw_headings(&self, buf: &mut Buffer, hits: &mut Hits, y: u16, tree: bool, t: &Theme) {
        let program = if tree { "Tree:" } else { "Program:" };
        // The numbers' headings stand one cell right of their columns and
        // show their shortcut letter.
        let headings = [
            (Some(self.pid_end - 4), "Pid:", false, Sort::Pid),
            (Some(self.prog_x), program, false, Sort::Program),
            (self.cmd_x, "Command:", false, Sort::Command),
            (self.threads_x, "Threads:", false, Sort::Threads),
            (self.user_x, "User:", false, Sort::User),
            (Some(self.mem_x + 1), "MemB", true, Sort::Memory),
            (self.gpu_x.map(|x| x + 1), "Gpu%", true, Sort::Gpu),
            (Some(self.cpu_x + 1), "Cpu%", true, Sort::Cpu),
        ];
        for (x, label, shortcut, sort) in headings {
            if let Some(x) = x {
                heading(buf, hits, (x, y), label, shortcut, sort, t);
            }
        }
    }
}

/// Two significant digits below 100, none from there: a CPU or GPU share.
fn share(v: f32) -> String {
    if v < 100.0 {
        format!("{v:.1}")
    } else {
        format!("{v:.0}")
    }
}

/// The rows of the process list, drawn one at a time.
struct ProcList<'a> {
    app: &'a App,
    theme: &'a Theme,
    cols: Columns,
    /// Left edge and width of the list, scroll bar included.
    x: u16,
    width: u16,
    /// The machine's core count, and what a process's CPU share is
    /// multiplied by to read per core (1 when it reads per machine).
    all_cores: f32,
    cpu_scale: f32,
}

impl ProcList<'_> {
    /// Draws row `i` of the view on screen row `y`.
    fn draw_row(&self, buf: &mut Buffer, hits: &mut Hits, i: usize, y: u16) {
        let (app, t, cols) = (self.app, self.theme, &self.cols);
        let row = &app.view[i];
        let Some(p) = app.proc(row.pid) else {
            return;
        };
        let line = Rect::new(self.x, y, self.width - 1, 1);
        hits.push((line, Hit::Row(i)));
        put_right(buf, cols.pid_end, y, &p.pid.to_string(), fg(t.fg));
        if app.tree {
            self.draw_tree_name(buf, hits, (i, row, p), y);
        } else {
            putn(buf, cols.prog_x, y, &p.name, cols.prog_w, fg(t.fg));
            if let Some(x) = cols.cmd_x {
                putn(buf, x, y, &p.cmd, cols.text_end - x, fg(t.inactive));
            }
        }
        if let Some(x) = cols.threads_x {
            put_right(buf, x + 8, y, &row.load.threads.to_string(), fg(t.fg));
        }
        if let Some(x) = cols.user_x {
            putn(buf, x, y, &p.user, 9, fg(t.inactive));
        }
        put_right(buf, cols.mem_x + 5, y, &human(row.load.mem as f64, &SHORT, true), fg(t.fg));
        // Blank for processes that cannot use the GPU at all.
        if let Some(x) = cols.gpu_x.filter(|_| p.drm || row.load.gpu > 0.0) {
            let color = if row.load.gpu < 0.05 { t.inactive } else { t.load.at(row.load.gpu) };
            put_right(buf, x + 5, y, &share(row.load.gpu), fg(color));
        }
        if let Some(x) = cols.graph_x {
            graph(buf, Rect::new(x, y, 5, 1), p.history(), 100.0, &t.load, t.inactive, false);
        }
        let cpu = row.load.cpu * self.cpu_scale;
        let color = if cpu < 0.05 { t.inactive } else { t.load.at(row.load.cpu * self.all_cores) };
        put_right(buf, cols.cpu_x + 5, y, &share(cpu), fg(color));
        if app.selected == Some(i) {
            let style = Style::new().fg(t.selected_fg).bg(t.selected_bg).add_modifier(Modifier::BOLD);
            buf.set_style(line, style);
        }
    }

    /// The program column in the tree, for the process `p` shown as row `i`:
    /// guides of the ancestors, the branch, a fold mark, the name, then the
    /// command.
    fn draw_tree_name(&self, buf: &mut Buffer, hits: &mut Hits, (i, row, p): (usize, &Row, &Proc), y: u16) {
        let (t, cols) = (self.theme, &self.cols);
        let room = |x: u16| cols.text_end.saturating_sub(x);
        let mut x = cols.prog_x;
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
    }
}

/// The scroll bar's arrows at the ends of the column `x` from `y`, `h` tall,
/// and its thumb between them.
fn scroll_bar(buf: &mut Buffer, (x, y, h): (u16, u16, u16), offset: usize, total: usize, rows: usize, t: &Theme) {
    put(buf, x, y, "↑", fg(t.hi));
    put(buf, x, y + h - 1, "↓", fg(t.hi));
    let track = h.saturating_sub(2) as usize;
    if total > rows && track > 0 {
        let at = offset * (track - 1) / (total - rows);
        put(buf, x, y + 1 + at as u16, "█", fg(t.fg));
    }
}

pub(super) fn proc_box(buf: &mut Buffer, r: Rect, app: &mut App, t: &Theme, hits: &mut Hits) {
    frame(buf, r, t.box_proc);
    app.proc_area = r;
    proc_top_bar(buf, r, app, t, hits);
    proc_bottom_bar(buf, r, app, t, hits);
    let (ix, mut iy, iw, mut ih) = inner(r);
    if iw < 30 || ih < 2 {
        return;
    }
    if app.info.is_some() && ih >= 11 {
        let used = info_pane(buf, Rect::new(ix, iy, iw, ih - 4), app, t, hits);
        iy += used;
        ih -= used;
    }

    let cols = Columns::new(ix, iw, app.gpu_procs, app.tree);
    cols.draw_headings(buf, hits, iy, app.tree, t);

    // Keep the selection on screen and the list full.
    let rows = (ih - 1) as usize;
    let total = app.view.len();
    app.page_rows = rows;
    if let Some(row) = app.selected {
        app.offset = app.offset.clamp((row + 1).saturating_sub(rows), row);
    }
    app.offset = app.offset.min(total.saturating_sub(rows));

    let all_cores = app.cores.len().max(1) as f32;
    let cpu_scale = if app.per_core { all_cores } else { 1.0 };
    let list = ProcList { app, theme: t, cols, x: ix, width: iw, all_cores, cpu_scale };
    for i in app.offset..total.min(app.offset + rows) {
        list.draw_row(buf, hits, i, iy + 1 + (i - app.offset) as u16);
    }
    scroll_bar(buf, (list.cols.scroll_x, iy, ih), app.offset, total, rows, t);
}
