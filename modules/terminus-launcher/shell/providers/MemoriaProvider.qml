import QtQuick
import Quickshell
import Quickshell.Io
import qs.modules.launcher
import "../modules/launcher/Item.js" as Schema

// Memoria ("@"): the clipboard history, from the bar's own ClipboardService over its msg interface:
//
//   clipboard-list                 JSON, newest first: { id, kind: text|image, mime, preview, bytes, captured_ms,
//                                  pinned, masked } (masked: the preview looked like a secret and was replaced)
//   clipboard-get <id>             the full text, or the path of an exported image
//   clipboard-copy-entry <id>      back onto the clipboard
//   clipboard-pin <id> on|off      pin / unpin
//   clipboard-remove <id>          delete
//
// Rows show the one-line preview and the age; ◆ marks pinned entries, ■ images. Masked entries stay masked in the
// list: their real text is fetched (clipboard-get) only for the selected row, into the detail pane, as is every
// entry's full text or image. Enter copies the entry back; Tab: copy, pin/unpin, delete.
//
// The running bar is asked for clipboard-list at startup and every time "@" is typed, so the mode becomes available
// without a launcher restart.
Provider {
    id: root

    mode: "memoria"
    prefix: "@"
    slow: true
    missing: root.barHasHistory ? "" : "needs the bar's clipboard-list command (not added yet)"

    property bool barHasHistory: false

    Process {
        id: check
        running: true
        command: ["sh", "-c", "terminus-bar msg --help 2>/dev/null | grep -q '^ *clipboard-list'"]
        onExited: code => {
            const has = code === 0
            if (has !== root.barHasHistory) {
                root.barHasHistory = has
                root.stale()
            }
        }
    }

    function recheck() {
        if (!check.running)
            check.running = true
    }

    // ===== the list =====

    property var listProc: null

    Component {
        id: runComponent

        Process {
            id: p
            property var done: null
            stdout: StdioCollector {
                onStreamFinished: {
                    if (p.done)
                        p.done(text)
                    p.done = null
                }
            }
            onExited: Qt.callLater(p.destroy)
        }
    }

    // Runs `terminus-bar msg <args>` and hands its stdout to done (unless cancelled through the returned process).
    function msg(args, done) {
        const p = runComponent.createObject(root, { done: done, command: ["terminus-bar", "msg"].concat(args) })
        p.running = true
        return p
    }

    function cancel() {
        if (root.listProc) {
            root.listProc.done = null
            root.listProc.running = false
            root.listProc = null
        }
    }

    function query(text, token, prefixed) {
        root.cancel()
        const q = text.trim().toLowerCase()
        root.listProc = root.msg(["clipboard-list"], out => {
            root.listProc = null
            let list = []
            try {
                const j = JSON.parse(out)
                if (Array.isArray(j))
                    list = j
            } catch (e) {
                root.results(token, [Schema.notice("memoria:bad", "unavailable", "the bar's clipboard-list did not answer with JSON")])
                return
            }
            root.total = list.length
            const shown = q === "" ? list : list.filter(c => (c.preview || "").toLowerCase().indexOf(q) >= 0)
            root.results(token, shown.length > 0
                ? shown.map(c => root.entryItem(c))
                : [Schema.notice("memoria:none", q === "" ? "the clipboard history is empty" : "nothing in the history matches " + q)])
        })
    }

    // ===== items =====

    function oneLine(s) {
        return (s || "").replace(/\s+/g, " ").trim()
    }

    // Runs a change, then asks the router to list again once the bar has done it.
    function change(args) {
        root.msg(args, () => root.stale())
    }

    // The selected row's full entry: its text (masked or not) as lines, or the exported image.
    function probe(c) {
        return done => {
            root.msg(["clipboard-get", c.id], out => {
                if (c.kind === "image") {
                    const path = out.trim()
                    done(path !== "" ? [{ label: "", text: "", image: path }] : [{ label: "content", text: "could not be read" }])
                    return
                }
                const lines = out.split("\n").slice(0, 12).map(l => l.replace(/\t/g, "    "))
                done([{ label: "", text: "" }].concat(lines.map(l => ({ label: "", text: l === "" ? " " : l }))))
            })
        }
    }

    function entryItem(c) {
        const image = c.kind === "image"
        const title = image ? "image · " + (c.mime || "") : (root.oneLine(c.preview) || "(empty)")
        return Schema.make({
            id: "clip:" + c.id,
            title: title,
            value: Schema.ago(c.captured_ms || 0, Date.now()),
            glyph: c.pinned ? "◆" : (image ? "■" : "·"),
            summary: c.masked ? "masked in the list: it looks like a secret" : "",
            detail: [
                { label: "type", text: (c.mime || c.kind || "unknown") },
                { label: "size", text: Schema.bytes(c.bytes || 0) },
                { label: "pinned", text: c.pinned ? "yes" : "no" }
            ],
            probe: root.probe(c),
            actions: [
                {
                    id: "copy", label: "copy", key: "c",
                    description: "put it back on the clipboard",
                    run: ctx => {
                        root.msg(["clipboard-copy-entry", c.id], () => {})
                        ctx.done("copied")
                    }
                },
                {
                    id: "pin", label: c.pinned ? "unpin" : "pin", key: "p",
                    description: c.pinned ? "let it age out of the history again" : "keep it: pinned entries survive clearing",
                    run: ctx => {
                        root.change(["clipboard-pin", c.id, c.pinned ? "off" : "on"])
                        ctx.back()
                        ctx.say(c.pinned ? "unpinned" : "pinned")
                    }
                },
                {
                    id: "delete", label: "delete", key: "d", danger: true,
                    description: "remove it from the history",
                    run: ctx => {
                        root.change(["clipboard-remove", c.id])
                        ctx.back()
                        ctx.say("deleted")
                    }
                }
            ]
        })
    }
}
