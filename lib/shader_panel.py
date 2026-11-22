#!/usr/bin/env python3
"""Searchable screen-shader picker on Hyprland (front end of terminus-screenfx)."""

import ctypes
import json
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlparse

from picker_style import hypr_json, read_theme

# Layer-shell must interpose GTK's Wayland calls before GTK is loaded.
ctypes.CDLL("libgtk4-layer-shell.so", mode=ctypes.RTLD_GLOBAL)
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")
from gi.repository import Gdk, Gio, GLib, GObject, Gtk, Gtk4LayerShell as LayerShell

BIN = Path(__file__).resolve().parent.parent / "bin"
# One picker UI for three engines; the profile (argv[1], default "shaders") picks the CLI and window identity.
PROFILES = {
    "shaders": {"app_id": "local.terminus.Shaders", "namespace": "terminus-shaders",
                "window_title": "Terminus-Est Shaders", "frame_title": "[ screen shaders ]",
                "cli": str(BIN / "terminus-screenfx"),
                "footer": "Enter  Use only this     Ctrl+Enter  Add to stack     Ctrl+Backspace  Off"
                          "     Tab  Category     Esc  Close"},
    "keysounds": {"app_id": "local.terminus.Keysounds", "namespace": "terminus-keysounds",
                 "window_title": "Terminus-Est Keyboard Sounds", "frame_title": "[ keyboard sounds ]",
                 "cli": str(BIN / "terminus-mechvibes"),
                 "placeholder": "search sound packs, authors, #category", "volume_keys": True, "delete_key": True,
                 "footer": "Enter  Use / set     Ctrl+Enter  Mute device, add mood     Ctrl+Backspace  Sounds off"
                           "     +/-  Volume     Ctrl+Delete  Delete pack     Tab  Category     Esc  Close"},
    "audio": {"app_id": "local.terminus.Audio", "namespace": "terminus-audio",
              "window_title": "Terminus-Est Audio", "frame_title": "[ audio effects ]",
              "cli": str(BIN / "terminus-audiofx"),
              "footer": "Enter  Use this effect     Ctrl+Enter  Toggle     Ctrl+Backspace  Off"
                        "     Tab  Category     Esc  Close"},
    "motion": {"app_id": "local.terminus.Motion", "namespace": "terminus-motion",
               "window_title": "Terminus-Est Motion", "frame_title": "[ motion ]",
               "cli": str(BIN / "terminus-motionfx"), "strip_label": "active::", "preview": True, "width": 1420,
               "placeholder": "search animations, effects, cursors, #category",
               "footer": "Enter  Use     Ctrl+Enter  Toggle, stay open     Ctrl+Backspace  Defaults"
                         "     Tab  Category     Esc  Close"},
}
ALL, ACTIVE = "\0all", "\0active"
VOLUME_KEYVALS = {Gdk.KEY_plus: "up", Gdk.KEY_equal: "up", Gdk.KEY_KP_Add: "up",
                  Gdk.KEY_minus: "down", Gdk.KEY_KP_Subtract: "down"}
PREVIEW_CSS = """
.preview-pane { border-left: 1px solid @line@; padding-left: 12px; }
.preview-screen { border: 1px solid @line@; background: #000000; }
"""


class Effect(GObject.Object):
    """One row of `terminus-screenfx list --json`; `version` bumps when it changes."""

    version = GObject.Property(type=int, default=0)

    def __init__(self, data):
        super().__init__()
        self.assign(data)

    def assign(self, data):
        self.d = data
        self.category_key = data["category"].lower()
        self.haystack = " ".join([data["label"], data["name"], data["description"],
                                  data["category"], *data["tags"]]).lower()

    def update(self, data):
        if data != self.d:
            self.assign(data)
            self.version += 1


def source_host(data):
    source = data.get("source") or "original"
    return urlparse(source).netloc or source


def make_label(css, text="", **kwargs):
    label = Gtk.Label(label=text, **kwargs)
    for name in css.split():
        label.add_css_class(name)
    return label


class ClipPlayer:
    """Loops a short clip in a Gtk.Picture: ffmpeg decodes raw frames into a pipe, a timer shows them.

    GTK here has no media backend, and the pipe's back-pressure keeps ffmpeg at the display rate.
    """

    WIDTH, HEIGHT, FPS = 480, 270, 30

    def __init__(self, picture):
        self.picture = picture
        self.path = ""
        self.proc = None
        self.timer = 0

    def play(self, path):
        if path == self.path and self.proc:
            return
        self.stop()
        self.path = path
        if not path:
            return
        self.proc = subprocess.Popen(
            ["ffmpeg", "-v", "quiet", "-stream_loop", "-1", "-i", path, "-an",
             "-vf", f"scale={self.WIDTH}:{self.HEIGHT},fps={self.FPS}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
        self.timer = GLib.timeout_add(1000 // self.FPS, self.tick)

    def tick(self):
        frame = self.proc.stdout.read(self.WIDTH * self.HEIGHT * 3)
        if len(frame) < self.WIDTH * self.HEIGHT * 3:
            self.timer = 0
            self.stop()
            return False
        self.picture.set_paintable(Gdk.MemoryTexture.new(
            self.WIDTH, self.HEIGHT, Gdk.MemoryFormat.R8G8B8, GLib.Bytes.new(frame), self.WIDTH * 3))
        return True

    def stop(self):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = 0
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc = None
        self.path = ""
        self.picture.set_paintable(None)


class ShaderPanel(Gtk.Application):
    def __init__(self, profile):
        super().__init__(application_id=profile["app_id"],
                         flags=Gio.ApplicationFlags.IS_SERVICE)
        self.profile = profile
        self.categories = []
        self.groups = []
        self.items = {}
        self.tokens = []
        self.category = ALL
        self.cat_rows = []
        self.queue = []
        self.busy = False
        self.refreshing = False
        self.refresh_again = False
        self.keep_selected = None
        self.extra_state = ""
        self.pending_delete = None
        self.player = None
        self.preview_timer = 0
        self.connect("startup", self.build_panel)
        self.connect("activate", self.activate_panel)

    # ---- construction -------------------------------------------------
    def build_panel(self, _app):
        self.hold()
        display = Gdk.Display.get_default()
        if not LayerShell.is_supported():
            raise RuntimeError("This display does not support GTK layer-shell")
        self.provider = Gtk.CssProvider()
        self.theme_stamp = None
        self.load_theme()
        Gtk.StyleContext.add_provider_for_display(display, self.provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.window = Gtk.ApplicationWindow(application=self, title=self.profile["window_title"])
        self.window.connect("close-request", self.close_panel)
        LayerShell.init_for_window(self.window)
        LayerShell.set_namespace(self.window, self.profile["namespace"])
        LayerShell.set_layer(self.window, LayerShell.Layer.OVERLAY)
        LayerShell.set_keyboard_mode(self.window, LayerShell.KeyboardMode.EXCLUSIVE)
        LayerShell.set_exclusive_zone(self.window, -1)

        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        panel.add_css_class("shader-panel")
        panel.set_size_request(self.profile.get("width", 1240), 700)
        wrap = Gtk.Box()
        wrap.add_css_class("frame-wrap")
        wrap.append(panel)
        frame = Gtk.Overlay(child=wrap)
        # Title and state sit on the top border line, covering it with the surface colour.
        title = make_label("frame-title", self.profile["frame_title"], halign=Gtk.Align.START, valign=Gtk.Align.START)
        self.state = make_label("frame-state", "", halign=Gtk.Align.END, valign=Gtk.Align.START)
        frame.add_overlay(title)
        frame.add_overlay(self.state)
        self.window.set_child(frame)

        self.entry = Gtk.SearchEntry(placeholder_text=self.profile.get("placeholder", "search effects, tags, #category"))
        self.entry.add_css_class("search")
        self.entry.connect("search-changed", self.search_changed)
        panel.append(self.entry)

        strip = Gtk.Box(spacing=10)
        strip.add_css_class("strip")
        strip.append(make_label("strip-label", self.profile.get("strip_label", "stack::")))
        self.chips = Gtk.Box(hexpand=True)
        strip.append(self.chips)
        self.clear_button = Gtk.Button(label="clear", focusable=False, focus_on_click=False)
        self.clear_button.add_css_class("clear")
        self.clear_button.connect("clicked", lambda *_: self.apply("off"))
        strip.append(self.clear_button)
        panel.append(strip)

        body = Gtk.Box(spacing=12, vexpand=True)
        panel.append(body)
        self.cat_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE, focusable=False)
        self.cat_list.add_css_class("cats")
        self.cat_list.connect("row-selected", self.category_selected)
        cat_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, focusable=False)
        cat_scroll.add_css_class("cat-pane")
        cat_scroll.set_size_request(200, -1)
        cat_scroll.set_child(self.cat_list)
        body.append(cat_scroll)

        self.store = Gio.ListStore(item_type=Effect)
        self.filter = Gtk.CustomFilter.new(self.item_visible)
        self.filtered = Gtk.FilterListModel(model=self.store, filter=self.filter)
        self.selection = Gtk.SingleSelection(model=self.filtered, autoselect=True)
        self.selection.connect("items-changed", lambda *_: self.update_empty())
        self.selection.connect("notify::selected-item", lambda *_: self.queue_preview())
        factory = Gtk.SignalListItemFactory()
        factory.connect("setup", self.row_setup)
        factory.connect("bind", self.row_bind)
        factory.connect("unbind", self.row_unbind)
        self.listview = Gtk.ListView(model=self.selection, factory=factory, focusable=False)
        self.listview.add_css_class("fx-list")
        scroll = Gtk.ScrolledWindow(hexpand=True, vexpand=True, focusable=False)
        scroll.set_child(self.listview)
        self.empty = make_label("empty", "No matching effects", vexpand=True, hexpand=True)
        self.empty.set_visible(False)
        stack = Gtk.Overlay(hexpand=True)
        stack.set_child(scroll)
        self.empty.set_halign(Gtk.Align.CENTER)
        self.empty.set_valign(Gtk.Align.CENTER)
        stack.add_overlay(self.empty)
        body.append(stack)
        if self.profile.get("preview"):
            body.append(self.build_preview())

        foot = Gtk.Box()
        self.footer = make_label("footer", self.profile["footer"], xalign=0, hexpand=True)
        self.status = make_label("status", "", xalign=1)
        foot.append(self.footer)
        foot.append(self.status)
        panel.append(foot)

        keys = Gtk.EventControllerKey()
        keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self.key_pressed)
        self.window.add_controller(keys)
        self.window.realize()
        self.refresh()

    def row_setup(self, _factory, list_item):
        row = Gtk.Box(spacing=12)
        row.append(make_label("emoji", ""))
        text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2, hexpand=True,
                       valign=Gtk.Align.CENTER)
        text.append(make_label("fx-name", "", xalign=0))
        text.append(make_label("fx-desc", "", xalign=0, hexpand=True, ellipsize=3, max_width_chars=12))
        row.append(text)
        row.append(make_label("meta", "", xalign=1, valign=Gtk.Align.CENTER, ellipsize=3,
                              width_chars=38, max_width_chars=38))
        row.append(make_label("meta", "", xalign=1, valign=Gtk.Align.CENTER, ellipsize=3,
                              width_chars=11, max_width_chars=11))
        marker = Gtk.Box(valign=Gtk.Align.CENTER, halign=Gtk.Align.END)
        marker.set_size_request(84, -1)
        row.append(marker)
        if self.profile.get("preview"):  # the preview pane carries category, source and licence instead
            meta = text.get_next_sibling()
            meta.set_visible(False)
            meta.get_next_sibling().set_visible(False)
        click = Gtk.GestureClick(button=1)
        click.connect("released", self.row_clicked, list_item)
        row.add_controller(click)
        list_item.set_child(row)

    def row_bind(self, _factory, list_item):
        item = list_item.get_item()
        list_item.handler = item.connect("notify::version", lambda *_: self.row_fill(list_item))
        self.row_fill(list_item)

    def row_unbind(self, _factory, list_item):
        list_item.get_item().disconnect(list_item.handler)

    def row_fill(self, list_item):
        d = list_item.get_item().d
        row = list_item.get_child()
        emoji = row.get_first_child()
        text = emoji.get_next_sibling()
        meta = text.get_next_sibling()
        source = meta.get_next_sibling()
        marker = source.get_next_sibling()
        emoji.set_label(d["emoji"])
        name = text.get_first_child()
        name.set_label(d["label"])
        if d["active"] and not d["shadowed"]:
            name.add_css_class("on-name")
        else:
            name.remove_css_class("on-name")
        text.get_last_child().set_label(d["description"])
        text.set_tooltip_text(f"{d['name']} · {d['license']} · {d['source']}")
        flags = [d["category"], *d["flags"]]
        meta.set_label(" · ".join(flags))
        source.set_label(source_host(d))
        while child := marker.get_first_child():
            marker.remove(child)
        if d["active"] and d["shadowed"]:
            marker.append(make_label("shadow", f"shadowed #{d['position']}"))
        elif d["active"]:
            marker.append(make_label("on", f"ON #{d['position']}"))

    def row_clicked(self, gesture, _presses, _x, _y, list_item):
        state = gesture.get_current_event_state()
        toggle = state & (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.SHIFT_MASK)
        self.selection.set_selected(list_item.get_position())
        self.choose(list_item.get_item(), bool(toggle))

    def load_theme(self):
        css, stamp = read_theme(PREVIEW_CSS if self.profile.get("preview") else "")
        if stamp != self.theme_stamp:
            self.theme_stamp = stamp
            self.provider.load_from_data(css)

    # ---- data ---------------------------------------------------------
    def run_fx(self, args, callback):
        try:
            proc = Gio.Subprocess.new(
                [self.profile["cli"], *args],
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_PIPE)
        except GLib.Error as error:
            callback(1, "", error.message)
            return

        def done(proc, result):
            try:
                _ok, out, err = proc.communicate_utf8_finish(result)
            except GLib.Error as error:
                callback(1, "", error.message)
                return
            callback(proc.get_exit_status(), out or "", err or "")
        proc.communicate_utf8_async(None, None, done)

    def refresh(self):
        if self.refreshing or self.busy:
            self.refresh_again = True
            return
        self.refreshing = True
        self.refresh_again = False
        self.run_fx(["list", "--json"], self.refreshed)

    def refreshed(self, status, out, err):
        self.refreshing = False
        if self.refresh_again:
            self.refresh()
            return
        try:
            if status:
                raise ValueError(err.strip() or f"exit {status}")
            data = json.loads(out)
        except ValueError as error:
            self.status.set_label(f"Could not load effects: {error}")
            return
        self.load(data)

    def load(self, data):
        effects = data["effects"]
        self.extra_state = f"{data['state']}  " if data.get("state") else ""  # e.g. "vol 80%" from the engine
        names = [e["name"] for e in effects]
        self.categories = data["categories"]
        self.groups = data.get("groups", [])
        if names == [i.d["name"] for i in self.store]:
            for item, d in zip(self.store, effects):
                item.update(d)
        else:
            self.store.splice(0, self.store.get_n_items(), [Effect(d) for d in effects])
        self.build_categories()
        self.build_chips(effects)
        self.refilter(reset=False)

    def build_categories(self):
        # A category named "Group · Name" whose Group is in self.groups is nested under one "Group" row.
        keys = [ALL, ACTIVE]
        for category in self.categories:
            group = self.group_of(category)
            if group and group not in keys:
                keys.append(group)
            keys.append(category)
        if keys != [key for key, *_ in self.cat_rows]:
            while row := self.cat_list.get_first_child():
                self.cat_list.remove(row)
            self.cat_rows = []
            for index, key in enumerate(keys):
                box = Gtk.Box(spacing=8)
                name = {ALL: "All", ACTIVE: "Active"}.get(key, key)
                group = self.group_of(key)
                nested = group is not None
                if nested:
                    name = key.split(" · ", 1)[1]
                    # Accent-coloured branch: one continuous trunk through the rows, a tick to each name;
                    # the last child's trunk stops at the tick.
                    last = index + 1 >= len(keys) or self.group_of(keys[index + 1]) != group
                    trunk = Gtk.Box(width_request=1, margin_start=10, halign=Gtk.Align.START)
                    trunk.add_css_class("tree-trunk")
                    if last:
                        trunk.set_valign(Gtk.Align.START)
                        trunk.set_size_request(1, 14)
                    tick = Gtk.Box(width_request=10, height_request=1, margin_start=10,
                                   halign=Gtk.Align.START, valign=Gtk.Align.CENTER)
                    tick.add_css_class("tree-tick")
                    branch = Gtk.Overlay(child=trunk)
                    branch.add_overlay(tick)
                    branch.set_margin_end(6)  # gap between the tick and the name
                    box.append(branch)
                pad = 5 if nested else 0  # nested rows have no CSS padding so the trunk can run through them
                box.append(Gtk.Label(label=name, xalign=0, hexpand=True, margin_top=pad, margin_bottom=pad))
                count = make_label("count", "")
                box.append(count)
                row = Gtk.ListBoxRow(child=box, focusable=False)
                if nested:
                    row.add_css_class("nested")
                self.cat_list.append(row)
                self.cat_rows.append((key, row, count))
            self.select_category(self.category if self.category in keys else ALL)

    def build_chips(self, effects):
        while chip := self.chips.get_first_child():
            self.chips.remove(chip)
        active = sorted((e for e in effects if e["active"]), key=lambda e: e["position"])
        for e in active:
            chip = make_label("chip", f"{e['position']}:{e['label'].lower()}")
            if e["shadowed"]:
                chip.add_css_class("shadowed")
                chip.set_tooltip_text("Shadowed by a later geometric effect")
            self.chips.append(chip)
        if not active:
            self.chips.append(make_label("none", "none"))
        self.clear_button.set_sensitive(bool(active))
        strip = self.profile.get("strip_label", "stack::")
        self.state.set_label(f"[ {self.extra_state}{strip}{len(active)}  effects::{len(effects)} ]")

    # ---- filtering ----------------------------------------------------
    def matches_text(self, item):
        for token in self.tokens:
            if token.startswith("#") and len(token) > 1:
                wanted = token[1:]
                if wanted == "active":
                    if not item.d["active"]:
                        return False
                elif wanted not in item.category_key.replace(" ", "").replace("&", ""):
                    return False
            elif token not in item.haystack:
                return False
        return True

    def group_of(self, category):
        """The group a "Group · Name" category is nested under, else None."""
        group, sep, _ = category.partition(" · ")
        return group if sep and group in self.groups else None

    def in_category(self, item, key):
        if key == ALL:
            return True
        if key == ACTIVE:
            return item.d["active"]
        if key in self.groups:
            return self.group_of(item.d["category"]) == key
        return item.d["category"] == key

    def matches_category(self, item):
        return self.in_category(item, self.category)

    def item_visible(self, item):
        return self.matches_text(item) and self.matches_category(item)

    def search_changed(self, entry):
        self.tokens = entry.get_text().lower().split()
        self.refilter(reset=True)

    def refilter(self, reset):
        selected = self.selection.get_selected_item()
        self.keep_selected = None if reset or selected is None else selected.d["name"]
        self.filter.changed(Gtk.FilterChange.DIFFERENT)
        self.update_counts()
        if self.keep_selected:
            for position, item in enumerate(self.filtered):
                if item.d["name"] == self.keep_selected:
                    self.selection.set_selected(position)
                    break
        elif self.filtered.get_n_items():
            self.selection.set_selected(0)
            self.listview.scroll_to(0, Gtk.ListScrollFlags.NONE, None)
        self.update_empty()

    def update_counts(self):
        matched = [i for i in self.store if self.matches_text(i)]
        for key, _row, label in self.cat_rows:
            label.set_label(str(sum(1 for i in matched if self.in_category(i, key))))

    def update_empty(self):
        self.empty.set_visible(self.filtered.get_n_items() == 0)

    def select_category(self, key):
        for k, row, _ in self.cat_rows:
            if k == key:
                self.cat_list.select_row(row)
                return

    def category_selected(self, _list, row):
        if row is None:
            return
        for key, candidate, _ in self.cat_rows:
            if candidate is row and key != self.category:
                self.category = key
                self.refilter(reset=True)
                return

    def cycle_category(self, step):
        keys = [key for key, *_ in self.cat_rows]
        if keys:
            self.select_category(keys[(keys.index(self.category) + step) % len(keys)])

    # ---- actions ------------------------------------------------------
    def choose(self, item, add):
        if item is None:
            return
        name = item.d["name"]
        if name.startswith("volume/"):  # a volume step: stay open so it can be repeated
            self.apply("volume", name.removeprefix("volume/"))
        elif add:
            self.apply("toggle", name)
        else:
            self.apply("set", name)
            self.close_panel()

    def apply(self, *args):
        self.queue.append(list(args))
        self.pump()

    def pump(self):
        if self.busy:
            return
        if not self.queue:
            self.status.set_label("")
            self.refresh()
            return
        self.busy = True
        self.status.set_label("Applying…")
        self.run_fx(self.queue.pop(0), self.applied)

    def applied(self, status, _out, err):
        self.busy = False
        if status:
            print(f"{Path(self.profile['cli']).name} failed: {err.strip()}", file=sys.stderr)
        self.pump()
        if status:
            self.status.set_label(err.strip().splitlines()[-1] if err.strip() else "Apply failed")

    # ---- window -------------------------------------------------------
    def target_monitor(self):
        try:
            monitors = hypr_json("monitors")
            window = hypr_json("activewindow")
            name = None
            if window.get("address"):
                name = next((m["name"] for m in monitors if m["id"] == window.get("monitor")), None)
            if name is None:
                name = next((m["name"] for m in monitors if m.get("focused")), None)
        except (OSError, subprocess.SubprocessError, ValueError):
            return None
        return next((m for m in Gdk.Display.get_default().get_monitors()
                     if m.get_connector() == name), None)

    def activate_panel(self, _app):
        if self.window.get_visible():
            self.close_panel()
            return
        LayerShell.set_monitor(self.window, self.target_monitor())
        self.category = ALL
        self.select_category(ALL)
        self.entry.set_text("")
        self.tokens = []
        self.refilter(reset=True)
        self.load_theme()
        self.window.present()
        self.entry.grab_focus()
        self.refresh()
        self.show_preview()

    # ---- preview ------------------------------------------------------
    def build_preview(self):
        pane = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        pane.add_css_class("preview-pane")
        picture = Gtk.Picture(content_fit=Gtk.ContentFit.COVER, can_shrink=True)
        picture.set_size_request(ClipPlayer.WIDTH, ClipPlayer.HEIGHT)
        self.preview_missing = make_label("empty", "no preview recorded")
        screen = Gtk.Overlay(child=picture, valign=Gtk.Align.START)
        screen.add_css_class("preview-screen")
        screen.add_overlay(self.preview_missing)
        pane.append(screen)
        self.preview_name = make_label("fx-name", "", xalign=0)
        self.preview_desc = make_label("fx-desc", "", xalign=0, wrap=True, max_width_chars=60)
        self.preview_meta = make_label("meta", "", xalign=0, wrap=True, max_width_chars=66)
        for label in (self.preview_name, self.preview_desc, self.preview_meta):
            pane.append(label)
        self.player = ClipPlayer(picture)
        return pane

    def queue_preview(self):
        """Start the clip once the selection rests, so arrowing through the list does not spawn decoders."""
        if not self.player:
            return
        if self.preview_timer:
            GLib.source_remove(self.preview_timer)
        self.preview_timer = GLib.timeout_add(120, self.show_preview)

    def show_preview(self):
        if self.preview_timer:
            GLib.source_remove(self.preview_timer)
            self.preview_timer = 0
        if not self.player or not self.window.get_visible():
            return False
        item = self.selection.get_selected_item()
        d = item.d if item else None
        clip = d.get("preview", "") if d else ""
        self.preview_missing.set_visible(bool(d) and not clip)
        self.player.play(clip)
        if not d:
            for label in (self.preview_name, self.preview_desc, self.preview_meta):
                label.set_label("")
            return False
        self.preview_name.set_label(f"{d['emoji']}  {d['label']}")
        self.preview_desc.set_label(d["description"])
        lines = [f"category  {d['category']}", f"replaces  other {d.get('slot', '')} entries",
                 f"source    {source_host(d)} · {d['license']}"]
        if "plugin" in d["flags"]:
            lines.append("needs     a Hyprland plugin (modules/hyprland-plugins)")
        self.preview_meta.set_label("\n".join(lines))
        return False

    def close_panel(self, *_args):
        if self.player:
            self.player.stop()
        self.window.set_visible(False)
        return True

    def move(self, delta):
        count = self.filtered.get_n_items()
        if not count:
            return
        position = min(max(self.selection.get_selected() + delta, 0), count - 1)
        self.selection.set_selected(position)
        self.listview.scroll_to(position, Gtk.ListScrollFlags.NONE, None)

    def delete_selected(self):
        """Ctrl+Delete twice on the same row deletes that sound pack; any other key cancels."""
        item = self.selection.get_selected_item()
        if item is None:
            return
        name = item.d["name"]
        if not name.startswith(("keyboard/", "mouse/")):
            self.status.set_label("Only installed sound packs can be deleted")
        elif self.pending_delete == name:
            self.pending_delete = None
            self.status.set_label("")
            self.apply("remove", name)
        else:
            self.pending_delete = name
            self.status.set_label(f"Press Ctrl+Delete again to delete {item.d['label']}")

    def key_pressed(self, _controller, keyval, _keycode, state):
        ctrl = bool(state & Gdk.ModifierType.CONTROL_MASK)
        confirming = self.profile.get("delete_key") and ctrl and keyval == Gdk.KEY_Delete
        if not confirming and self.pending_delete:
            self.pending_delete = None
            self.status.set_label("")
        shift = bool(state & Gdk.ModifierType.SHIFT_MASK)
        if keyval == Gdk.KEY_Escape:
            self.close_panel()
        elif keyval in (Gdk.KEY_Down, Gdk.KEY_KP_Down) or (ctrl and keyval in (Gdk.KEY_j, Gdk.KEY_n)):
            self.move(1)
        elif keyval in (Gdk.KEY_Up, Gdk.KEY_KP_Up) or (ctrl and keyval in (Gdk.KEY_k, Gdk.KEY_p)):
            self.move(-1)
        elif keyval == Gdk.KEY_Page_Down:
            self.move(8)
        elif keyval == Gdk.KEY_Page_Up:
            self.move(-8)
        elif keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            self.choose(self.selection.get_selected_item(), ctrl or shift)
        elif self.profile.get("volume_keys") and (ctrl or not self.entry.get_text()) and keyval in VOLUME_KEYVALS:
            self.apply("volume", VOLUME_KEYVALS[keyval])
        elif confirming:
            self.delete_selected()
        elif keyval in (Gdk.KEY_BackSpace, Gdk.KEY_Delete) and ctrl:
            self.apply("off")
        elif keyval == Gdk.KEY_Tab:
            self.cycle_category(1)
        elif keyval == Gdk.KEY_ISO_Left_Tab:
            self.cycle_category(-1)
        else:
            return False
        return True


def main():
    profile = PROFILES.get(sys.argv[1] if len(sys.argv) > 1 else "shaders")
    if profile is None:
        print(f"usage: shader_panel.py [{'|'.join(PROFILES)}]", file=sys.stderr)
        return 2
    return ShaderPanel(profile).run([sys.argv[0]])


if __name__ == "__main__":
    raise SystemExit(main())
