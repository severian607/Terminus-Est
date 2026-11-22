#!/usr/bin/env python3
"""Unicode entry and Markdown practice truth tables on Hyprland."""

import ctypes
import subprocess
import sys
from pathlib import Path

from picker_style import hypr_json, read_theme

# Layer-shell must interpose GTK's Wayland calls before GTK is loaded.
ctypes.CDLL("libgtk4-layer-shell.so", mode=ctypes.RTLD_GLOBAL)
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")
from gi.repository import Gdk, Gio, GLib, Gtk
from gi.repository import Gtk4LayerShell as LayerShell

ALT_SHORTCUTS = tuple(
    line.split("\t")
    for line in (Path(__file__).resolve().parent.parent / "config/symbol-shortcuts.tsv")
    .read_text(encoding="utf-8")
    .splitlines()
)
SYMBOLS = {key: symbol for key, symbol, _ in ALT_SHORTCUTS}
# The TSV has no group column (the Hyprland binder reads it too), so groups live here.
GROUPS = {
    "logic": "5 6 7 f t b d a l",
    "sets": "u i x s o e m",
    "numbers": "z n r q c",
    "relations": "< > = j",
}
GROUP_OF = {key: group for group, keys in GROUPS.items() for key in keys.split()}
ALL = "\0all"
MODIFIERS = (
    Gdk.ModifierType.SHIFT_MASK
    | Gdk.ModifierType.CONTROL_MASK
    | Gdk.ModifierType.ALT_MASK
    | Gdk.ModifierType.SUPER_MASK
    | Gdk.ModifierType.META_MASK
    | Gdk.ModifierType.HYPER_MASK
)
RELEASED_MASKS = {
    "Shift_L": Gdk.ModifierType.SHIFT_MASK,
    "Shift_R": Gdk.ModifierType.SHIFT_MASK,
    "Control_L": Gdk.ModifierType.CONTROL_MASK,
    "Control_R": Gdk.ModifierType.CONTROL_MASK,
    "Alt_L": Gdk.ModifierType.ALT_MASK,
    "Alt_R": Gdk.ModifierType.ALT_MASK,
    "Super_L": Gdk.ModifierType.SUPER_MASK,
    "Super_R": Gdk.ModifierType.SUPER_MASK,
    "Meta_L": Gdk.ModifierType.META_MASK,
    "Meta_R": Gdk.ModifierType.META_MASK,
    "Hyper_L": Gdk.ModifierType.HYPER_MASK,
    "Hyper_R": Gdk.ModifierType.HYPER_MASK,
}
SYMBOL_FOOTER = "key  insert on release    Tab  category    F1  truth table    Esc  cancel    ·  clipboard untouched"
TABLE_FOOTER = "2-4  insert table    Backspace  symbols    Esc  cancel"
SYMBOL_MODE = ("press a key to insert", "   ·   alt+key works anywhere without this panel")
TABLE_MODE = ("choose the number of variables", "   ·   pasted as an editable markdown table, replaces clipboard")
COLUMNS = 4
PANEL_WIDTH, PANEL_HEIGHT = 1240, 560
# The panel shrinks to leave this much of the monitor free, and drops to fewer grid columns below these widths.
SCREEN_MARGIN_X_PX, SCREEN_MARGIN_Y_PX = 48, 96
WIDE_GRID_MIN_WIDTH_PX, MEDIUM_GRID_MIN_WIDTH_PX = 1100, 800
TABLES = (
    ("2", "p, q", "4 rows"),
    ("3", "p, q, r", "8 rows"),
    ("4", "p, q, r, s", "16 rows"),
)

# Only what the pickers' shared stylesheet lacks; the glyph font is the one proportional exception.
EXTRA_CSS = """
.mode-line { border: 1px solid @line@; padding: 4px 8px; min-height: 24px; }
.mode-main { color: @fg@; font-size: 13px; }
.sym-cell { padding: 6px 8px; border-left: 2px solid transparent;
            border-bottom: 1px solid @line@; border-radius: 0; }
.sym-cell.pressed { background: @accent@; border-left-color: @accent@; }
.sym-cell.pressed label { color: @bg@; border-color: @bg@; }
.sym-key { color: @accent@; border: 1px solid @line@; border-radius: 0;
           min-width: 20px; padding: 1px 5px; font-size: 12px; }
.sym-glyph { font-family: 'Noto Sans Math', 'DejaVu Sans', sans-serif; font-size: 20px; min-width: 26px; }
.sym-desc { color: @muted@; font-size: 12px; }
.sym-name { color: @fg@; font-size: 14px; }
"""


def truth_table(variable_count):
    """Return every assignment, true first, with three blank working columns."""
    headers = list("pqrs"[:variable_count]) + ["Step 1", "Step 2", "Result"]
    rows = [headers, ["---"] * len(headers)]
    rows.extend(
        ["F" if assignment & (1 << bit) else "T" for bit in reversed(range(variable_count))] + ["", "", ""]
        for assignment in range(1 << variable_count)
    )
    return "\n\n" + "\n".join("| " + " | ".join(row) + " |" for row in rows) + "\n\n"


def make_label(css, text="", **kwargs):
    label = Gtk.Label(label=text, **kwargs)
    for name in css.split():
        label.add_css_class(name)
    return label


def key_caption(key):
    return "⇧" + key.lower() if key.isupper() else key


class SymbolPanel(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="local.terminus.Symbols", flags=Gio.ApplicationFlags.IS_SERVICE)
        self.target = None
        self.delivering = False
        self.selected = None
        self.selected_keycode = None
        self.key_released = False
        self.table_mode = False
        self.category = ALL
        self.cells = {}
        self.columns = COLUMNS
        self.table_cells = {}
        self.cat_rows = []
        self.connect("startup", self.build_panel)
        self.connect("activate", self.activate_panel)

    def build_panel(self, _app):
        self.hold()
        display = Gdk.Display.get_default()
        if not LayerShell.is_supported():
            raise RuntimeError("This display does not support GTK layer-shell")
        self.provider = Gtk.CssProvider()
        self.theme_stamp = None
        self.load_theme()
        Gtk.StyleContext.add_provider_for_display(display, self.provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.window = Gtk.ApplicationWindow(application=self, title="Terminus-Est Symbols")
        self.window.connect("close-request", self.cancel_panel)
        LayerShell.init_for_window(self.window)
        LayerShell.set_namespace(self.window, "terminus-symbols")
        LayerShell.set_layer(self.window, LayerShell.Layer.OVERLAY)
        LayerShell.set_keyboard_mode(self.window, LayerShell.KeyboardMode.EXCLUSIVE)
        LayerShell.set_exclusive_zone(self.window, -1)

        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        panel.add_css_class("shader-panel")
        panel.set_size_request(PANEL_WIDTH, PANEL_HEIGHT)
        self.panel = panel
        wrap = Gtk.Box()
        wrap.add_css_class("frame-wrap")
        wrap.append(panel)
        frame = Gtk.Overlay(child=wrap)
        # Title and counters sit on the top border line, covering it with the surface colour.
        title = make_label("frame-title", "[ symbols ]", halign=Gtk.Align.START, valign=Gtk.Align.START)
        self.state = make_label("frame-state", "", halign=Gtk.Align.END, valign=Gtk.Align.START)
        frame.add_overlay(title)
        frame.add_overlay(self.state)
        self.window.set_child(frame)

        # No search entry: keys insert directly, so the mode line takes its place.
        mode = Gtk.Box()
        mode.add_css_class("mode-line")
        self.mode_main = make_label("mode-main", "", xalign=0)
        self.mode_hint = make_label("strip-label", "", xalign=0, hexpand=True, ellipsize=3)
        mode.append(self.mode_main)
        mode.append(self.mode_hint)
        panel.append(mode)

        self.stack = Gtk.Stack(vexpand=True, hexpand=True)
        panel.append(self.stack)
        self.stack.add_named(self.build_symbols_page(), "symbols")
        self.stack.add_named(self.build_tables_page(), "tables")

        self.footer = make_label("footer", SYMBOL_FOOTER, xalign=0, hexpand=True)
        panel.append(self.footer)

        keys = Gtk.EventControllerKey()
        keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self.key_pressed)
        keys.connect("key-released", self.key_released_event)
        self.window.add_controller(keys)
        self.window.realize()
        self.show_tables(False)

    def build_symbols_page(self):
        body = Gtk.Box(spacing=12)
        self.cat_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE, focusable=False)
        self.cat_list.add_css_class("cats")
        self.cat_list.connect("row-selected", self.category_selected)
        cat_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, focusable=False)
        cat_scroll.add_css_class("cat-pane")
        cat_scroll.set_size_request(200, -1)
        cat_scroll.set_child(self.cat_list)
        body.append(cat_scroll)

        groups = []
        for key, *_ in ALT_SHORTCUTS:
            group = GROUP_OF.get(key, "algebra")
            if group not in groups:
                groups.append(group)
        for category in (ALL, *groups):
            box = Gtk.Box(spacing=8)
            box.append(Gtk.Label(label="all" if category == ALL else category, xalign=0, hexpand=True))
            count = sum(1 for key, *_ in ALT_SHORTCUTS if category == ALL or GROUP_OF.get(key, "algebra") == category)
            box.append(make_label("count", str(count)))
            row = Gtk.ListBoxRow(child=box, focusable=False)
            self.cat_list.append(row)
            self.cat_rows.append((category, row))

        self.grid = Gtk.Grid(column_homogeneous=True, valign=Gtk.Align.START, hexpand=True)
        for key, symbol, description in ALT_SHORTCUTS:
            cell = Gtk.Box(spacing=10, hexpand=True)
            cell.add_css_class("sym-cell")
            cell.set_tooltip_text(f"Alt+{key_caption(key)}")
            cell.append(make_label("sym-key", key_caption(key)))
            cell.append(make_label("sym-glyph", symbol))
            cell.append(
                make_label("sym-desc", description.lower(), xalign=0, hexpand=True, ellipsize=3, max_width_chars=10)
            )
            self.cells[key] = (GROUP_OF.get(key, "algebra"), cell)
        scroll = Gtk.ScrolledWindow(hexpand=True, vexpand=True, focusable=False, hscrollbar_policy=Gtk.PolicyType.NEVER)
        scroll.set_child(self.grid)
        body.append(scroll)
        return body

    def build_tables_page(self):
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.START)
        for key, variables, rows in TABLES:
            cell = Gtk.Box(spacing=14)
            cell.add_css_class("sym-cell")
            cell.append(make_label("sym-key", key))
            cell.append(make_label("sym-name", variables, xalign=0, width_chars=14))
            cell.append(make_label("sym-desc", f"·  {rows}", xalign=0, hexpand=True))
            page.append(cell)
            self.table_cells[key] = cell
        for text in (
            "t/f combinations filled in   ·   step 1, step 2 and result left blank",
            "pastes an editable markdown table   ·   replaces clipboard",
        ):
            page.append(make_label("strip-label", text, xalign=0, margin_top=14, ellipsize=3))
        return page

    def load_theme(self):
        css, stamp = read_theme(EXTRA_CSS)
        if stamp != self.theme_stamp:
            self.theme_stamp = stamp
            self.provider.load_from_data(css)

    # ---- categories ---------------------------------------------------
    def fill_grid(self):
        while child := self.grid.get_first_child():
            self.grid.remove(child)
        shown = [cell for group, cell in self.cells.values() if self.category == ALL or group == self.category]
        for index, cell in enumerate(shown):
            self.grid.attach(cell, index % self.columns, index // self.columns, 1, 1)
        self.state.set_label(f"symbols::{len(shown)}")

    def fit_to_monitor(self, monitor):
        """Size the panel to the target monitor's logical geometry and reflow the grid."""
        width, height = PANEL_WIDTH, PANEL_HEIGHT
        if monitor is not None:
            geometry = monitor.get_geometry()
            width = min(width, geometry.width - SCREEN_MARGIN_X_PX)
            height = min(height, geometry.height - SCREEN_MARGIN_Y_PX)
        columns = 4 if width >= WIDE_GRID_MIN_WIDTH_PX else 3 if width >= MEDIUM_GRID_MIN_WIDTH_PX else 2
        self.panel.set_size_request(width, height)
        if columns != self.columns:
            self.columns = columns
            self.fill_grid()

    def select_category(self, category):
        for key, row in self.cat_rows:
            if key == category:
                self.cat_list.select_row(row)
                return

    def category_selected(self, _list, row):
        for key, candidate in self.cat_rows:
            if candidate is row:
                self.category = key
                self.fill_grid()
                return

    def cycle_category(self, step):
        keys = [key for key, _ in self.cat_rows]
        self.select_category(keys[(keys.index(self.category) + step) % len(keys)])

    # ---- window -------------------------------------------------------
    def activate_panel(self, _app):
        if self.window.get_visible():
            self.cancel_panel()
            return
        self.load_theme()
        if self.delivering:
            return
        target = hypr_json("activewindow")
        if not target.get("address"):
            return
        self.target = target
        self.mark_pressed(None)
        self.selected = None
        self.selected_keycode = None
        self.key_released = False
        self.select_category(ALL)
        self.show_tables(False)
        monitor_name = next((m["name"] for m in hypr_json("monitors") if m["id"] == target.get("monitor")), None)
        monitor = next((m for m in Gdk.Display.get_default().get_monitors() if m.get_connector() == monitor_name), None)
        self.fit_to_monitor(monitor)
        LayerShell.set_monitor(self.window, monitor)
        self.window.present()

    def show_tables(self, enabled):
        self.table_mode = enabled
        self.stack.set_visible_child_name("tables" if enabled else "symbols")
        main, hint = TABLE_MODE if enabled else SYMBOL_MODE
        self.mode_main.set_label(main)
        self.mode_hint.set_label(hint)
        self.footer.set_label(TABLE_FOOTER if enabled else SYMBOL_FOOTER)
        if enabled:
            self.state.set_label(f"tables::{len(TABLES)}")
        else:
            self.fill_grid()

    def mark_pressed(self, cell):
        for _group, other in self.cells.values():
            other.remove_css_class("pressed")
        for other in self.table_cells.values():
            other.remove_css_class("pressed")
        if cell is not None:
            cell.add_css_class("pressed")

    def cancel_panel(self, *_args):
        self.selected = None
        self.mark_pressed(None)
        self.window.set_visible(False)
        return True

    def insert_symbol(self):
        self.delivering = True
        self.window.set_visible(False)
        Gdk.Display.get_default().sync()
        # Process the unmap round trip, then type without an artificial delay.
        GLib.idle_add(self.deliver_symbol, self.selected, self.target["address"], self.table_mode)

    def deliver_symbol(self, symbol, target_address, paste=False):
        try:
            if hypr_json("activewindow").get("address") != target_address:
                print("Symbol insertion cancelled: the original window lost focus.", file=sys.stderr)
            elif paste:
                Gdk.Display.get_default().get_clipboard().set(symbol)
                subprocess.run(["wtype", "-M", "ctrl", "-k", "v", "-m", "ctrl"], check=True, timeout=3)
            else:
                subprocess.run(["wtype", "--", symbol], check=True, timeout=3)
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            print(f"Symbol insertion failed: {error}", file=sys.stderr)
        finally:
            self.selected = None
            self.delivering = False
        return GLib.SOURCE_REMOVE

    def key_pressed(self, _controller, keyval, keycode, state):
        if keyval == Gdk.KEY_Escape:
            self.cancel_panel()
            return True
        if self.selected is not None:
            return True
        char = chr(Gdk.keyval_to_unicode(keyval)).lower()
        if state & Gdk.ModifierType.SHIFT_MASK and char.upper() in SYMBOLS:
            char = char.upper()
        if state & (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK):
            return True
        if self.table_mode:
            if keyval == Gdk.KEY_BackSpace:
                self.show_tables(False)
                return True
            if char not in self.table_cells:
                return True
            self.selected = truth_table(int(char))
            self.mark_pressed(self.table_cells[char])
            self.footer.set_label(f"release keys to insert a {char}-variable truth table")
        elif keyval == Gdk.KEY_F1:
            self.show_tables(True)
            return True
        elif keyval == Gdk.KEY_Tab:
            self.cycle_category(1)
            return True
        elif keyval == Gdk.KEY_ISO_Left_Tab:
            self.cycle_category(-1)
            return True
        elif char in SYMBOLS:
            group, cell = self.cells[char]
            if self.category not in (ALL, group):
                self.select_category(ALL)
            self.selected = SYMBOLS[char]
            self.mark_pressed(cell)
            self.footer.set_label(f"release keys to insert {self.selected}")
        else:
            return True
        self.selected_keycode = keycode
        return True

    def key_released_event(self, _controller, keyval, keycode, state):
        if self.selected is None:
            return
        if keycode == self.selected_keycode:
            self.key_released = True
        # GDK release events may still report the just-released modifier.
        held = state & MODIFIERS & ~RELEASED_MASKS.get(Gdk.keyval_name(keyval), 0)
        if self.key_released and not held:
            self.insert_symbol()


def main():
    return SymbolPanel().run([sys.argv[0]])


if __name__ == "__main__":
    raise SystemExit(main())
