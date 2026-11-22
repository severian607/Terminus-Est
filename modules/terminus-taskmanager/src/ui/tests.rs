//! Renders fixed `App`s to a buffer and compares them with `snapshots/`:
//! every cell's symbol and style, and the click targets. Run with
//! `UPDATE_SNAPSHOTS=1` to rewrite the files after an intended change.

use super::draw;
use crate::app::{App, Hit, Shown, Sort, SIGNAL_ROWS};
use crate::theme::Theme;
use ratatui::{backend::TestBackend, Terminal};
use std::{collections::BTreeMap, fmt::Write as _, fs, path::PathBuf};

const STYLE_CHARS: &[u8] = b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

fn describe(hit: &Hit) -> String {
    match hit {
        Hit::Key(code) => format!("key {code:?}"),
        Hit::Interval(by) => format!("interval {by}"),
        Hit::Sort(sort) => format!("sort {}", sort.label()),
        Hit::Row(row) => format!("row {row}"),
        Hit::Fold(row) => format!("fold {row}"),
        Hit::Signal(index) => format!("signal {index}"),
    }
}

/// The text of a drawn frame: the symbols, a map of the styles (one letter
/// per distinct style, legend below) and the click targets in order.
fn snapshot(app: &mut App, width: u16, height: u16) -> String {
    let mut terminal = Terminal::new(TestBackend::new(width, height)).unwrap();
    let theme = Theme::fixture();
    terminal.draw(|f| draw(f, app, &theme)).unwrap();
    let buffer = terminal.backend().buffer();

    let (mut symbols, mut styles) = (String::new(), String::new());
    let mut legend: BTreeMap<String, char> = BTreeMap::new();
    for y in 0..height {
        for x in 0..width {
            let cell = &buffer[(x, y)];
            symbols.push_str(cell.symbol());
            let style = format!("{:?} {:?} {:?}", cell.fg, cell.bg, cell.modifier);
            let next = legend.len();
            let letter = *legend.entry(style).or_insert(STYLE_CHARS[next] as char);
            styles.push(letter);
        }
        symbols.push('\n');
        styles.push('\n');
    }
    let mut out = format!("{symbols}\n{styles}\n");
    let mut legend: Vec<_> = legend.into_iter().map(|(style, letter)| (letter, style)).collect();
    legend.sort();
    for (letter, style) in legend {
        let _ = writeln!(out, "{letter} = {style}");
    }
    out.push('\n');
    for (area, hit) in &app.hits {
        let _ = writeln!(out, "{},{} {}x{} {}", area.x, area.y, area.width, area.height, describe(hit));
    }
    out
}

fn check(name: &str, app: &mut App, width: u16, height: u16) {
    let got = snapshot(app, width, height);
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("src/ui/snapshots").join(format!("{name}.txt"));
    if std::env::var_os("UPDATE_SNAPSHOTS").is_some() {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, &got).unwrap();
        return;
    }
    let want = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    assert!(
        got == want,
        "{name}: frame differs from {}; rerun with UPDATE_SNAPSHOTS=1 if intended\n{got}",
        path.display()
    );
}

#[test]
fn flat_list_wide() {
    let mut app = App::fixture();
    app.selected = Some(2);
    check("flat_wide", &mut app, 160, 48);
}

#[test]
fn tree_with_info_pane() {
    let mut app = App::fixture();
    app.tree = true;
    app.per_core = true;
    app.rebuild_view();
    app.selected = app.view.iter().position(|row| row.pid == 1402);
    app.fixture_info(1402);
    app.fold(app.selected.unwrap() + 1, Some(true));
    check("tree_info", &mut app, 160, 48);
}

#[test]
fn filter_and_follow_medium() {
    let mut app = App::fixture();
    app.filter = "work".to_string();
    app.filtering = true;
    app.sort = Sort::Memory;
    app.reverse = true;
    app.rebuild_view();
    app.selected = Some(5);
    app.follow = app.view.get(5).map(|row| row.pid);
    app.paused = true;
    check("filter_medium", &mut app, 100, 30);
}

#[test]
fn minimum_size() {
    let mut app = App::fixture();
    app.selected = Some(0);
    check("minimum", &mut app, 80, 24);
}

#[test]
fn tall_narrow_columns() {
    let mut app = App::fixture();
    app.sort = Sort::Gpu;
    app.rebuild_view();
    check("tall_narrow", &mut app, 124, 70);
}

#[test]
fn boxes_hidden() {
    let mut app = App::fixture();
    app.shown = Shown { cpu: true, mem: false, net: true, proc: true, gpu: false };
    check("hidden_mem_gpu", &mut app, 120, 40);
    app.shown = Shown { cpu: false, mem: true, net: false, proc: false, gpu: true };
    check("only_gpu_mem", &mut app, 120, 40);
    app.shown = Shown { cpu: false, mem: false, net: false, proc: false, gpu: false };
    check("none", &mut app, 120, 40);
}

#[test]
fn too_small() {
    check("too_small", &mut App::fixture(), 60, 20);
}

#[test]
fn confirm_dialog() {
    let mut app = App::fixture();
    app.selected = Some(1);
    app.ask(libc::SIGKILL);
    check("dialog_confirm", &mut app, 120, 40);
}

#[test]
fn signal_dialog_with_error() {
    let mut app = App::fixture();
    app.selected = Some(3);
    app.pick();
    app.pick_move(SIGNAL_ROWS as isize + 1);
    check("dialog_pick", &mut app, 120, 40);
    if let Some(dialog) = &mut app.dialog {
        dialog.error = Some("Operation not permitted".to_string());
    }
    check("dialog_pick_error", &mut app, 120, 40);
}
