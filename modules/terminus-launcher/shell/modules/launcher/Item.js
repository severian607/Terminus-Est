.pragma library

// The one item schema every provider returns and the screen renders. The screen reads only these fields; make()
// fills in the ones a provider leaves out (all but id and title).
//
//   id        string   unique and stable across queries ("app:firefox.desktop", "win:0x55d0…")
//   title     string   the row's text; also the detail pane's section label
//   value     string   right-hand value ("ws 2", "4.2 KiB", "12m ago"); "" for none
//   glyph     string   one cell before the title: the row's state ("●" running, "○" not) or source ("=", "!")
//   key       string   optional: in an action menu, the key that runs this row ("f", "3")
//   meter     number   optional 0..1, drawn as a 5-cell ■■■·· meter; -1 or absent for none
//   slot      string   optional pinned digit shown before the glyph
//   summary   string   optional prose at the top of the detail pane
//   detail    array    [{ label, text }] rows of the detail pane
//   list      array    optional [{ label, text }] shown under the detail rows as long as there is room
//                      (a running app's windows: { label: "ws 2", text: <title> })
//   probe     function optional (done) => void; asked only for the selected row, calls done([{ label, text }]) with
//                      rows that are slow to get (memory)
//   actions   array    see below; actions[0] is Enter, the one with shift: true is Shift+Enter, Tab lists them all
//   danger    bool     the row is dangerous (reboot, kill): drawn in the warning tone
//   pinnable  bool     Ctrl+1-9 can pin it to a slot; its provider's resolve(id) must find it again later
//   inert     bool     a notice row: selectable for reading, no actions
//
// An action:
//
//   id, label    "focus", "move to workspace"
//   key          one character that runs it inside the action menu
//   shift        bool: Shift+Enter runs it from the list
//   danger       bool: drawn in the warning tone
//   confirm      bool: the first Enter only asks, the second runs it
//   confirmText  optional: what the hint row says while it asks
//   remember     bool: running it records the item in the launch log (frecency, query -> pick)
//   description  string shown in the detail pane while the action is selected
//   run(ctx)     does it; ctx.done(message) closes after showing message, ctx.say(message) only shows it
//   menu()       instead of run: returns items for a level deeper (workspaces, slots)

// An item from the fields a provider sets; the rest take the neutral values below.
function make(fields) {
    var it = {
        value: "",
        glyph: "",
        meter: -1,
        slot: "",
        summary: "",
        detail: [],
        list: [],
        actions: [],
        danger: false,
        pinnable: false,
        inert: false
    };
    for (var k in fields)
        it[k] = fields[k];
    return it;
}

function notice(id, title, summary) {
    return make({ id: "notice:" + id, title: title, glyph: "!", summary: summary || "", inert: true });
}

// The rows of an action menu for `item`: one per action, keyed by the action's key.
function actionItems(item) {
    var out = [];
    for (var i = 0; i < item.actions.length; i++) {
        var a = item.actions[i];
        out.push(make({
            id: item.id + "#" + a.id,
            title: a.label,
            value: a.menu ? "›" : "",
            glyph: a.key || "·",
            key: a.key || "",
            summary: a.description || "",
            detail: item.detail,
            list: item.list || [],
            actions: [a],
            danger: !!a.danger
        }));
    }
    return out;
}

function meterText(fraction, cells) {
    var n = cells || 5;
    var f = Math.max(0, Math.min(n, Math.round(fraction * n)));
    var on = "", off = "";
    for (var i = 0; i < f; i++)
        on += "■";
    for (var j = f; j < n; j++)
        off += "·";
    return { on: on, off: off };
}

// "3h ago" for a ms timestamp; "never" for 0.
function ago(t, now) {
    if (!t)
        return "never";
    var m = Math.floor((now - t) / 60000);
    if (m < 1)
        return "just now";
    if (m < 60)
        return m + "m ago";
    if (m < 1440)
        return Math.floor(m / 60) + "h ago";
    return Math.floor(m / 1440) + "d ago";
}

function bytes(n) {
    var units = ["B", "KiB", "MiB", "GiB", "TiB"];
    var i = 0;
    while (n >= 1024 && i < units.length - 1) {
        n /= 1024;
        i++;
    }
    return (i === 0 ? n : n.toFixed(n < 10 ? 1 : 0)) + " " + units[i];
}
