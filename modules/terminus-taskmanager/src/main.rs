//! Terminus-TaskManager: terminal setup, the sample-and-redraw loop, and what keys and the mouse do.

mod app;
mod draw;
mod gpu;
mod sys;
mod theme;
mod ui;

use app::{App, Ask, Hit, Sort, INTERVAL_STEP_MS, SIGNAL_ROWS};
use ratatui::{
    crossterm::{
        event::{
            self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers,
            MouseButton, MouseEvent, MouseEventKind,
        },
        execute,
    },
    layout::Position,
    DefaultTerminal,
};
use std::{
    io,
    time::{Duration, Instant},
};

fn main() -> io::Result<()> {
    // The control-center panel draws square corners, as its btop did.
    if std::env::args().any(|arg| arg == "--square") {
        draw::square_corners();
    }
    let mut theme = theme::Theme::load();
    let mut app = App::new();
    // Rates need two samples; take the second before the first frame.
    std::thread::sleep(Duration::from_millis(200));
    app.tick();

    let mut terminal = ratatui::init();
    // ratatui's panic hook restores the screen; the mouse needs releasing too.
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let _ = execute!(io::stdout(), DisableMouseCapture);
        hook(info);
    }));
    let result = execute!(io::stdout(), EnableMouseCapture).and_then(|()| run(&mut terminal, &mut app, &mut theme));
    let _ = execute!(io::stdout(), DisableMouseCapture);
    ratatui::restore();
    result
}

fn run(terminal: &mut DefaultTerminal, app: &mut App, theme: &mut theme::Theme) -> io::Result<()> {
    let mut sampled = Instant::now();
    let mut redraw = true;
    let mut themed = theme::Theme::changed();
    loop {
        if redraw {
            terminal.draw(|f| ui::draw(f, app, theme))?;
        }
        let due = sampled + Duration::from_millis(app.interval_ms);
        redraw = true;
        if event::poll(due.saturating_duration_since(Instant::now()))? {
            let quit = match event::read()? {
                // What a host sends to have the screen painted afresh;
                // Terminus-Bar does after every resize of its panel.
                Event::Key(key) if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('r') => {
                    terminal.clear()?;
                    false
                }
                Event::Key(key) if key.kind == KeyEventKind::Press => on_key(app, key),
                Event::Mouse(mouse) if !matches!(mouse.kind, MouseEventKind::Moved) => on_mouse(app, mouse),
                Event::Resize(..) => false,
                // Pointer motion arrives constantly and changes nothing.
                _ => {
                    redraw = false;
                    false
                }
            };
            if quit {
                return Ok(());
            }
        }
        if Instant::now() >= due {
            app.tick();
            // Terminus-Bar rewrites the theme when the scheme changes.
            let now = theme::Theme::changed();
            if now != themed {
                (themed, *theme) = (now, theme::Theme::load());
            }
            sampled = Instant::now();
            redraw = true;
        }
    }
}

/// Handles one key press; true means quit.
fn on_key(app: &mut App, key: KeyEvent) -> bool {
    if key.modifiers.contains(KeyModifiers::CONTROL) {
        // No other chord means anything; none may act as its letter.
        return key.code == KeyCode::Char('c');
    }
    if app.dialog.is_some() {
        on_dialog_key(app, key.code);
        return false;
    }
    if app.filtering {
        on_filter_key(app, key.code);
        return false;
    }
    let quit = on_list_key(app, key.code);
    app.refollow();
    quit
}

/// Keys while a dialog is open: confirm, move through the signal list, or
/// any other key to dismiss it.
fn on_dialog_key(app: &mut App, code: KeyCode) {
    let Some(dialog) = &app.dialog else {
        return;
    };
    let picking = matches!(dialog.ask, Ask::Pick(_));
    let column = SIGNAL_ROWS as isize;
    match code {
        // A failed send is only acknowledged.
        _ if dialog.error.is_some() => app.dialog = None,
        KeyCode::Enter => app.confirm(),
        KeyCode::Char('y') if !picking => app.confirm(),
        KeyCode::Up if picking => app.pick_move(-1),
        KeyCode::Down if picking => app.pick_move(1),
        KeyCode::Left if picking => app.pick_move(-column),
        KeyCode::Right if picking => app.pick_move(column),
        _ => app.dialog = None,
    }
}

/// Keys while the filter is being typed.
fn on_filter_key(app: &mut App, code: KeyCode) {
    match code {
        KeyCode::Enter => app.filtering = false,
        KeyCode::Esc => {
            app.filtering = false;
            app.filter.clear();
        }
        KeyCode::Backspace => {
            app.filter.pop();
        }
        KeyCode::Char(c) => app.filter.push(c),
        _ => return,
    }
    app.rebuild_view();
}

/// `+` and `-`: in the tree they open and close the selected branch, as in
/// btop; elsewhere they change the update interval.
fn open_or_step(app: &mut App, open: bool) {
    match app.selected.filter(|_| app.tree) {
        Some(row) => app.fold(row, Some(!open)),
        None => app.adjust_interval(if open { INTERVAL_STEP_MS } else { -INTERVAL_STEP_MS }),
    }
}

/// Escape backs out one step: the info pane, then the selection, then the
/// filter.
fn back_out(app: &mut App) {
    if app.info.take().is_some() || app.selected.take().is_some() || app.filter.is_empty() {
        return;
    }
    app.filter.clear();
    app.rebuild_view();
}

/// Keys of the process list; true means quit.
fn on_list_key(app: &mut App, code: KeyCode) -> bool {
    let page = app.page_rows.max(1) as isize;
    match code {
        KeyCode::Char('q') => return true,
        KeyCode::Char('+' | '=') => open_or_step(app, true),
        KeyCode::Char('-') => open_or_step(app, false),
        KeyCode::Char(' ') => {
            if let Some(row) = app.selected.filter(|_| app.tree) {
                app.fold(row, None);
            }
        }
        KeyCode::Char('b') => app.cycle_iface(-1),
        KeyCode::Char('n') => app.cycle_iface(1),
        KeyCode::Char('f') => app.filtering = true,
        KeyCode::Char('o') => app.per_core = !app.per_core,
        KeyCode::Char('e') => {
            app.tree = !app.tree;
            app.rebuild_view();
        }
        KeyCode::Char('r') => {
            app.reverse = !app.reverse;
            app.rebuild_view();
        }
        KeyCode::Left => app.set_sort(app.sort.step(-1)),
        KeyCode::Right => app.set_sort(app.sort.step(1)),
        KeyCode::Up => app.select(-1),
        KeyCode::Down => app.select(1),
        KeyCode::PageUp => app.select(-page),
        KeyCode::PageDown => app.select(page),
        KeyCode::Home => app.selected = app.selected.map(|_| 0),
        KeyCode::End => app.selected = app.view.len().checked_sub(1),
        KeyCode::Char('t') => app.ask(libc::SIGTERM),
        KeyCode::Char('k') => app.ask(libc::SIGKILL),
        KeyCode::Char('s') => app.pick(),
        KeyCode::Char('p' | 'P') => app.paused = !app.paused,
        // One key per sort that matters, instead of stepping with the arrows.
        KeyCode::Char('c' | 'C') => app.set_sort(Sort::Cpu),
        KeyCode::Char('m' | 'M') => app.set_sort(Sort::Memory),
        KeyCode::Char('g' | 'G') => app.set_sort(Sort::Gpu),
        KeyCode::Enter => app.toggle_info(),
        // btop's key; f is the filter.
        KeyCode::Char('F') => app.toggle_follow(),
        KeyCode::Esc => back_out(app),
        _ => {}
    }
    false
}

/// Handles a click or the wheel; true means quit.
fn on_mouse(app: &mut App, mouse: MouseEvent) -> bool {
    let at = Position::new(mouse.column, mouse.row);
    match mouse.kind {
        MouseEventKind::Down(MouseButton::Left) => {
            let Some(hit) = app.hits.iter().rev().find(|(area, _)| area.contains(at)).map(|(_, hit)| *hit) else {
                return false;
            };
            // A click anywhere ends typing into the filter.
            app.filtering = false;
            match hit {
                Hit::Key(code) => return on_key(app, KeyEvent::new(code, KeyModifiers::NONE)),
                Hit::Interval(by) => app.adjust_interval(by),
                Hit::Sort(sort) => app.set_sort(sort),
                Hit::Row(row) => app.selected = Some(row),
                Hit::Fold(row) => {
                    app.selected = Some(row);
                    app.refollow();
                    app.fold(row, None);
                }
                Hit::Signal(index) => app.pick_send(index),
            }
        }
        MouseEventKind::ScrollUp | MouseEventKind::ScrollDown if app.dialog.is_none() && app.proc_area.contains(at) => {
            app.scroll(if mouse.kind == MouseEventKind::ScrollUp { -1 } else { 1 });
        }
        _ => {}
    }
    app.refollow();
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::Dialog;

    fn press(app: &mut App, code: KeyCode) -> bool {
        on_key(app, KeyEvent::new(code, KeyModifiers::NONE))
    }

    #[test]
    fn escape_backs_out_one_step_at_a_time() {
        let mut app = App::fixture();
        app.filter = "work".to_string();
        app.rebuild_view();
        app.selected = Some(2);
        app.fixture_info(app.view[2].pid);

        press(&mut app, KeyCode::Esc);
        assert!(app.info.is_none() && app.selected.is_some() && !app.filter.is_empty());
        press(&mut app, KeyCode::Esc);
        assert!(app.selected.is_none() && !app.filter.is_empty());
        press(&mut app, KeyCode::Esc);
        assert!(app.filter.is_empty());
    }

    #[test]
    fn plus_and_minus_step_the_interval_in_the_list_and_fold_in_the_tree() {
        let mut app = App::fixture();
        press(&mut app, KeyCode::Char('+'));
        assert_eq!(app.interval_ms, 2100);
        press(&mut app, KeyCode::Char('-'));
        press(&mut app, KeyCode::Char('-'));
        assert_eq!(app.interval_ms, 1900);

        app.tree = true;
        app.rebuild_view();
        app.selected = app.view.iter().position(|row| row.kids);
        let rows = app.view.len();
        press(&mut app, KeyCode::Char('-'));
        assert!(app.view.len() < rows, "the branch closes");
        assert_eq!(app.interval_ms, 1900);
        press(&mut app, KeyCode::Char('+'));
        assert_eq!(app.view.len(), rows, "and opens again");
    }

    #[test]
    fn typing_edits_the_filter_until_enter() {
        let mut app = App::fixture();
        press(&mut app, KeyCode::Char('f'));
        assert!(app.filtering);
        for c in "fire".chars() {
            press(&mut app, KeyCode::Char(c));
        }
        press(&mut app, KeyCode::Backspace);
        assert_eq!(app.filter, "fir");
        let names: Vec<_> = app.view.iter().filter_map(|row| app.proc(row.pid)).map(|p| p.name.as_str()).collect();
        assert_eq!(names, ["firefox", "Web Content", "Web Content"], "by cpu, firefox first");
        press(&mut app, KeyCode::Enter);
        assert!(!app.filtering && app.filter == "fir");
        // Back in the list, the same letters are commands again.
        press(&mut app, KeyCode::Char('p'));
        assert!(app.paused);
    }

    #[test]
    fn dialog_keys_move_the_cursor_and_any_other_key_dismisses() {
        let mut app = App::fixture();
        app.selected = Some(0);
        app.pick();
        let Some(Dialog { ask: Ask::Pick(start), .. }) = app.dialog else {
            panic!("the signal list opens");
        };
        press(&mut app, KeyCode::Down);
        assert!(matches!(app.dialog, Some(Dialog { ask: Ask::Pick(at), .. }) if at == start + 1));
        press(&mut app, KeyCode::Right);
        assert!(matches!(app.dialog, Some(Dialog { ask: Ask::Pick(at), .. }) if at == start + 1 + SIGNAL_ROWS));
        press(&mut app, KeyCode::Char('x'));
        assert!(app.dialog.is_none());
    }

    #[test]
    fn control_chords_never_act_as_their_letter() {
        let mut app = App::fixture();
        let chord = |code| KeyEvent::new(code, KeyModifiers::CONTROL);
        assert!(!on_key(&mut app, chord(KeyCode::Char('p'))));
        assert!(!app.paused);
        assert!(on_key(&mut app, chord(KeyCode::Char('c'))));
        assert!(press(&mut app, KeyCode::Char('q')));
    }
}
