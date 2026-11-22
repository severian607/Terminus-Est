mod app;
mod draw;
mod gpu;
mod sys;
mod theme;
mod ui;

use app::{App, Ask, Hit, Sort, SIGNAL_ROWS};
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
    if let Some(dialog) = &app.dialog {
        let picking = matches!(dialog.ask, Ask::Pick(_));
        match key.code {
            // A failed send is only acknowledged.
            _ if dialog.error.is_some() => app.dialog = None,
            KeyCode::Enter => app.confirm(),
            KeyCode::Char('y') if !picking => app.confirm(),
            KeyCode::Up if picking => app.pick_move(-1),
            KeyCode::Down if picking => app.pick_move(1),
            KeyCode::Left if picking => app.pick_move(-(SIGNAL_ROWS as isize)),
            KeyCode::Right if picking => app.pick_move(SIGNAL_ROWS as isize),
            _ => app.dialog = None,
        }
        return false;
    }
    if app.filtering {
        match key.code {
            KeyCode::Enter => app.filtering = false,
            KeyCode::Esc => {
                app.filtering = false;
                app.filter.clear();
            }
            KeyCode::Backspace => {
                app.filter.pop();
            }
            KeyCode::Char(c) => app.filter.push(c),
            _ => return false,
        }
        app.rebuild_view();
        return false;
    }
    let page = app.page_rows.max(1) as isize;
    // In the tree, + and - open and close the selected branch, as in btop.
    let branch = app.selected.filter(|_| app.tree);
    match key.code {
        KeyCode::Char('q') => return true,
        KeyCode::Char('+' | '=') => match branch {
            Some(row) => app.fold(row, Some(false)),
            None => app.adjust_interval(100),
        },
        KeyCode::Char('-') => match branch {
            Some(row) => app.fold(row, Some(true)),
            None => app.adjust_interval(-100),
        },
        KeyCode::Char(' ') => {
            if let Some(row) = branch {
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
        KeyCode::Left | KeyCode::Right => {
            app.sort = app.sort.step(if key.code == KeyCode::Left { -1 } else { 1 });
            app.rebuild_view();
        }
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
        // Escape backs out one step: the info pane, the selection, the filter.
        KeyCode::Esc if app.info.take().is_none() && app.selected.take().is_none() && !app.filter.is_empty() => {
            app.filter.clear();
            app.rebuild_view();
        }
        _ => {}
    }
    app.refollow();
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
