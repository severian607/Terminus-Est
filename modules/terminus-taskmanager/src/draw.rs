//! Drawing primitives in btop's vocabulary: rounded boxes with titles cut
//! into the border, block meters and braille graphs.

use crate::theme::Gradient;
use ratatui::{
    buffer::Buffer,
    layout::Rect,
    style::{Color, Modifier, Style},
};
use std::sync::atomic::{AtomicBool, Ordering};

static SQUARE: AtomicBool = AtomicBool::new(false);

/// Makes every border from here on square-cornered.
pub fn square_corners() {
    SQUARE.store(true, Ordering::Relaxed);
}

pub fn fg(c: Color) -> Style {
    Style::new().fg(c)
}

pub fn bold(c: Color) -> Style {
    Style::new().fg(c).add_modifier(Modifier::BOLD)
}

fn width(s: &str) -> u16 {
    s.chars().count() as u16
}

/// Writes `s` at most `max` cells wide, clipped to the buffer. Returns the
/// column after the text.
pub fn putn(buf: &mut Buffer, x: u16, y: u16, s: &str, max: u16, style: Style) -> u16 {
    let a = buf.area;
    if x < a.x || x >= a.right() || y < a.y || y >= a.bottom() {
        return x;
    }
    buf.set_stringn(x, y, s, max.min(a.right() - x) as usize, style).0
}

/// One single-width character, skipping cells outside the buffer. The
/// cheap path for graphs, rules and meters, which draw thousands a frame
/// and need no grapheme segmentation.
fn glyph(buf: &mut Buffer, x: u16, y: u16, c: char, style: Style) {
    if let Some(cell) = buf.cell_mut((x, y)) {
        cell.set_char(c).set_style(style);
    }
}

pub fn put(buf: &mut Buffer, x: u16, y: u16, s: &str, style: Style) -> u16 {
    putn(buf, x, y, s, u16::MAX, style)
}

/// Writes `s` so that it ends just before column `end`.
pub fn put_right(buf: &mut Buffer, end: u16, y: u16, s: &str, style: Style) {
    put(buf, end.saturating_sub(width(s)), y, s, style);
}

pub fn hline(buf: &mut Buffer, x: u16, y: u16, w: u16, c: Color) {
    for i in 0..w {
        glyph(buf, x + i, y, '─', fg(c));
    }
}

pub fn vline(buf: &mut Buffer, x: u16, y: u16, h: u16, c: Color) {
    for i in 0..h {
        glyph(buf, x, y + i, '│', fg(c));
    }
}

/// A border around `r`, rounded unless `square_corners` was called.
pub fn frame(buf: &mut Buffer, r: Rect, c: Color) {
    if r.width < 2 || r.height < 2 {
        return;
    }
    let (right, bottom) = (r.right() - 1, r.bottom() - 1);
    hline(buf, r.x + 1, r.y, r.width - 2, c);
    hline(buf, r.x + 1, bottom, r.width - 2, c);
    vline(buf, r.x, r.y + 1, r.height - 2, c);
    vline(buf, right, r.y + 1, r.height - 2, c);
    let corners =
        if SQUARE.load(Ordering::Relaxed) { ["┌", "┐", "└", "┘"] } else { ["╭", "╮", "╰", "╯"] };
    put(buf, r.x, r.y, corners[0], fg(c));
    put(buf, right, r.y, corners[1], fg(c));
    put(buf, r.x, bottom, corners[2], fg(c));
    put(buf, right, bottom, corners[3], fg(c));
}

/// The cells inside a border: x, y, width, height.
pub fn inner(r: Rect) -> (u16, u16, u16, u16) {
    (r.x + 1, r.y + 1, r.width.saturating_sub(2), r.height.saturating_sub(2))
}

pub type Seg<'s> = (&'s str, Style);

/// Titles cut into one border row, `┐like this┌`, packed from both ends.
/// A title that no longer fits is left out.
pub struct Bar<'b> {
    buf: &'b mut Buffer,
    y: u16,
    left: u16,
    right: u16,
    line: Color,
    caps: (&'static str, &'static str),
}

impl<'b> Bar<'b> {
    /// Titles on the row `y` between columns `left` and `right` (exclusive).
    pub fn new(buf: &'b mut Buffer, y: u16, left: u16, right: u16, line: Color, bottom: bool) -> Bar<'b> {
        let caps = if bottom { ("┘", "└") } else { ("┐", "┌") };
        Bar { buf, y, left, right, line, caps }
    }

    pub fn top(buf: &'b mut Buffer, r: Rect, line: Color) -> Bar<'b> {
        Bar::new(buf, r.y, r.x + 2, r.right().saturating_sub(1), line, false)
    }

    pub fn bottom(buf: &'b mut Buffer, r: Rect, line: Color) -> Bar<'b> {
        Bar::new(buf, r.bottom().saturating_sub(1), r.x + 2, r.right().saturating_sub(1), line, true)
    }

    fn fits(&self, segs: &[Seg]) -> Option<u16> {
        let w = 2 + segs.iter().map(|(s, _)| width(s)).sum::<u16>();
        (self.left + w <= self.right).then_some(w)
    }

    fn draw(&mut self, mut x: u16, segs: &[Seg]) {
        x = put(self.buf, x, self.y, self.caps.0, fg(self.line));
        for (s, style) in segs {
            x = put(self.buf, x, self.y, s, *style);
        }
        put(self.buf, x, self.y, self.caps.1, fg(self.line));
    }

    /// Adds a title at the left; returns the cells its text took.
    pub fn left(&mut self, segs: &[Seg]) -> Option<Rect> {
        let w = self.fits(segs)?;
        self.draw(self.left, segs);
        self.left += w;
        Some(Rect::new(self.left - w + 1, self.y, w - 2, 1))
    }

    /// Adds a title at the right; returns the cells its text took.
    pub fn right(&mut self, segs: &[Seg]) -> Option<Rect> {
        let w = self.fits(segs)?;
        self.right -= w;
        self.draw(self.right, segs);
        Some(Rect::new(self.right + 1, self.y, w - 2, 1))
    }
}

/// btop's meter: `w` blocks, the filled ones in the colour of the reading.
pub fn meter(buf: &mut Buffer, x: u16, y: u16, w: u16, pct: f32, g: &Gradient, empty: Color) {
    let filled = g.at(pct);
    for i in 0..w {
        let step = ((i + 1) as f32 * 100.0 / w as f32).round();
        glyph(buf, x + i, y, '■', fg(if pct >= step { filled } else { empty }));
    }
}

/// A meter in the graphs' braille: a solid band as long as the reading, to
/// half a cell, over a baseline in `empty`.
pub fn gauge(buf: &mut Buffer, x: u16, y: u16, w: u16, pct: f32, g: &Gradient, empty: Color) {
    let halves = (pct.clamp(0.0, 100.0) * w as f32 / 50.0).round() as u16;
    let filled = fg(g.at(pct));
    for i in 0..w {
        match halves.saturating_sub(2 * i) {
            0 => glyph(buf, x + i, y, '⣀', fg(empty)),
            1 => glyph(buf, x + i, y, '⡇', filled),
            _ => glyph(buf, x + i, y, '⣿', filled),
        }
    }
}

/// One braille cell holding two samples, each 0..=4 dots tall, growing up
/// from the bottom or (`down`) hanging from the top.
fn braille(left: u8, right: u8, down: bool) -> char {
    const UP: ([u32; 5], [u32; 5]) = ([0, 0x40, 0x44, 0x46, 0x47], [0, 0x80, 0xa0, 0xb0, 0xb8]);
    const DOWN: ([u32; 5], [u32; 5]) = ([0, 0x01, 0x03, 0x07, 0x47], [0, 0x08, 0x18, 0x38, 0xb8]);
    let (l, r) = if down { DOWN } else { UP };
    char::from_u32(0x2800 | l[left as usize] | r[right as usize]).unwrap_or(' ')
}

/// btop's braille graph of the newest samples of `data` (scaled so `max` is
/// full height), newest at the right, two samples per cell. `invert` hangs
/// it from the top, for the lower half of a mirrored pair. The baseline is
/// always drawn where there is data. As in btop the colour follows the
/// height, so the tips of high columns change colour; a one-row graph takes
/// the colour of each reading, and columns that read nothing are in `idle`.
pub fn graph(buf: &mut Buffer, r: Rect, data: &[f32], max: f32, g: &Gradient, idle: Color, invert: bool) {
    let (w, h) = (r.width as usize, r.height as usize);
    if w == 0 || h == 0 || max <= 0.0 {
        return;
    }
    let pct = |v: f32| (v * 100.0 / max).clamp(0.0, 100.0);
    // Sample index of the left half of column 0; negative while the history
    // is shorter than the graph.
    let first = data.len() as isize - 2 * w as isize;
    for col in 0..w {
        let sample = |i: isize| (i >= 0).then(|| pct(data[i as usize]));
        let at = first + 2 * col as isize;
        let (left, right) = (sample(at), sample(at + 1));
        if left.is_none() && right.is_none() {
            continue;
        }
        let peak = left.unwrap_or(0.0).max(right.unwrap_or(0.0));
        for row in 0..h {
            // Band 0 touches the baseline.
            let band = if invert { row } else { h - 1 - row };
            let (low, high) = (100.0 * band as f32 / h as f32, 100.0 * (band + 1) as f32 / h as f32);
            let level = |v: Option<f32>| -> u8 {
                let Some(v) = v else { return 0 };
                let dots = if v >= high {
                    4
                } else if v <= low {
                    0
                } else {
                    ((v - low) * 4.0 / (high - low) + 0.1).round() as u8
                };
                if band == 0 {
                    dots.clamp(1, 4)
                } else {
                    dots.min(4)
                }
            };
            let (l, rr) = (level(left), level(right));
            if l == 0 && rr == 0 {
                continue;
            }
            // A row has the colour of the level at its middle, or of the
            // reading that ends inside it. Bold thickens the dots where
            // the terminal draws braille from a font.
            let color = bold(if peak < 1.0 { idle } else { g.at(peak.min((low + high) / 2.0)) });
            glyph(buf, r.x + col as u16, r.y + row as u16, braille(l, rr, invert), color);
        }
    }
}

/// A dotted line across `r` at the height a reading of half the scale
/// reaches, to go behind a graph drawn there afterwards (`invert` as for
/// the graph). Returns its row.
pub fn half_line(buf: &mut Buffer, r: Rect, invert: bool, c: Color) -> u16 {
    // One dot per cell, in the cell's left column; dot rows bottom to top.
    const DOT: [u32; 4] = [0x40, 0x04, 0x02, 0x01];
    let reached = (2 * r.height).saturating_sub(1);
    let (cell, dot) = (reached / 4, (reached % 4) as usize);
    let (y, bits) = if invert { (r.y + cell, DOT[3 - dot]) } else { (r.bottom().saturating_sub(1 + cell), DOT[dot]) };
    let mark = char::from_u32(0x2800 | bits).unwrap_or(' ');
    for x in r.x..r.right() {
        glyph(buf, x, y, mark, fg(c));
    }
    y
}
