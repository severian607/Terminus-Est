import QtQuick
import Quickshell
import Quickshell.Io
import qs.services
import qs.modules.common
import qs.modules.launcher
import "../modules/common/functions/Fuzzy.js" as Fuzzy
import "../modules/launcher/Item.js" as Schema

// Archivum ("/"): files and folders by name, with fd, in three roots: ~/Projects, ~/.config (hidden files and
// folders included there, .git internals not) and the rest of home (hidden and git-ignored paths skipped, fd's
// defaults). Ranked by how well the name matches, then shallower paths, ~/Projects above ~/.config above the rest,
// and ~/Backups (dated copies) lower still. Enter opens with the default app (xdg-open).
//
// The selected row is probed (scripts/probe/file.sh): its MIME type, the apps registered for it (the "open with"
// menu) and, for text, its first lines in the detail pane.
//
// Actions (Tab): open, open with, reveal in folder, copy path.
Provider {
    id: root

    mode: "archivum"
    prefix: "/"
    slow: true
    missing: root.hasFd ? "" : "fd is not installed (pacman -S fd)"

    property bool hasFd: true
    readonly property string home: Quickshell.env("HOME")

    Process {
        running: true
        command: ["sh", "-c", "command -v fd"]
        onExited: code => root.hasFd = code === 0
    }

    // ~/Projects, then ~/.config (with hidden paths), then the rest of home; at most 200 hits from each. The home
    // search never enters ~/.config itself (it skips hidden paths), so nothing is listed twice.
    readonly property string search:
        "fd -F -i --color never --max-results 200 -- \"$1\" \"$HOME/Projects\" 2>/dev/null; " +
        "fd -F -i --color never --max-results 200 --hidden --exclude .git -- \"$1\" \"$HOME/.config\" 2>/dev/null; " +
        "fd -F -i --color never --max-results 200 --exclude Projects -- \"$1\" \"$HOME\" 2>/dev/null"

    property var proc: null

    Component {
        id: fdComponent

        Process {
            id: p
            property int token: 0
            property string q: ""
            stdout: StdioCollector {
                onStreamFinished: {
                    if (p.token !== 0)
                        root.answer(p.token, p.q, text)
                }
            }
            onExited: {
                if (root.proc === p)
                    root.proc = null
                Qt.callLater(p.destroy)
            }
        }
    }

    function cancel() {
        if (root.proc) {
            root.proc.token = 0
            root.proc.running = false
            root.proc = null
        }
    }

    function query(text, token, prefixed) {
        root.cancel()
        const q = text.trim()
        if (q === "") {
            root.results(token, [Schema.notice("archivum:empty", "type part of a file name", "searches ~/Projects, ~/.config, then the rest of home")])
            return
        }
        root.proc = fdComponent.createObject(root, { token: token, q: q, command: ["sh", "-c", root.search, "sh", q] })
        root.proc.running = true
    }

    function tilde(path) {
        return path.startsWith(root.home) ? "~" + path.slice(root.home.length) : path
    }

    function answer(token, q, out) {
        const ql = q.toLowerCase()
        const seen = ({})
        const scored = []
        for (const line of out.split("\n")) {
            if (line === "" || seen[line])
                continue
            seen[line] = true
            const dir = line.endsWith("/")
            const path = dir ? line.slice(0, -1) : line
            const name = path.slice(path.lastIndexOf("/") + 1)
            const s = Fuzzy.substring(ql, name.toLowerCase())
            const depth = path.split("/").length
            // Live work first: ~/Projects, then ~/.config; ~/Backups (dated copies of everything) down.
            const place = path.startsWith(root.home + "/Projects/") ? 40
                : path.startsWith(root.home + "/.config/") ? 25
                : path.startsWith(root.home + "/Backups/") ? -80 : 0
            scored.push({ s: (s > 0 ? s : 200) - depth * 8 + (dir ? 5 : 0) + place, path: path, name: name, dir: dir })
        }
        scored.sort((a, b) => b.s - a.s)
        const items = scored.slice(0, 60).map(x => root.fileItem(x.path, x.name, x.dir))
        root.results(token, items.length > 0 ? items : [Schema.notice("archivum:none", "no file named like " + q, "fd found nothing under ~/Projects, ~/.config or ~")])
    }

    // One probe process at a time; a newer one silences the older.
    property var probeProc: null

    Component {
        id: probeComponent

        Process {
            id: pp
            property var done: null
            stdout: StdioCollector {
                onStreamFinished: {
                    if (pp.done)
                        pp.done(text)
                    pp.done = null
                }
            }
            onExited: {
                if (root.probeProc === pp)
                    root.probeProc = null
                Qt.callLater(pp.destroy)
            }
        }
    }

    function probe(path, info) {
        return done => {
            if (root.probeProc) {
                root.probeProc.done = null
                root.probeProc.running = false
            }
            root.probeProc = probeComponent.createObject(root, {
                done: text => {
                    const lines = text.split("\n")
                    const sep = lines.indexOf("---")
                    info.mime = lines[0] || "unknown"
                    info.apps = (lines[1] || "").split(" ").filter(s => s !== "")
                    const preview = sep >= 0 ? lines.slice(sep + 1).filter((l, i, a) => i < a.length - 1 || l !== "") : []
                    const rows = [{ label: "type", text: info.mime }]
                    if (preview.length > 0)
                        rows.push({ label: "", text: "" })
                    for (const l of preview)
                        rows.push({ label: "", text: l === "" ? " " : l })
                    done(rows)
                },
                command: ["sh", Directories.script("probe/file.sh"), path]
            })
            root.probeProc.running = true
        }
    }

    // gio launch wants the .desktop file itself: the first one found on the usual data dirs.
    readonly property string launchWith:
        "for d in \"$HOME/.local/share/applications\" /usr/local/share/applications /usr/share/applications " +
        "\"$HOME/.local/share/flatpak/exports/share/applications\" /var/lib/flatpak/exports/share/applications; do " +
        "[ -f \"$d/$1\" ] && exec gio launch \"$d/$1\" \"$2\"; done; exit 1"

    function openWithMenu(path, info) {
        if (info.apps === null)
            return [Schema.notice("archivum:apps", "still reading the file type", "select the row a moment, then try again")]
        const out = []
        for (const id of info.apps) {
            const e = DesktopEntries.byId(id.replace(/\.desktop$/, ""))
            const name = ((e && e.name) || id.replace(/\.desktop$/, "")).toLowerCase()
            const n = out.length + 1
            out.push(Schema.make({
                id: "with:" + id,
                title: name,
                value: n === 1 ? "default" : "",
                glyph: n <= 9 ? String(n) : "·",
                key: n <= 9 ? String(n) : "",
                summary: "open " + root.tilde(path) + " with " + name,
                actions: [{
                    id: "with",
                    label: "open",
                    run: ctx => {
                        Quickshell.execDetached(["sh", "-c", root.launchWith, "sh", id, path])
                        ctx.done("opening with " + name + "…")
                    }
                }]
            }))
        }
        return out.length > 0 ? out : [Schema.notice("archivum:noapps", "no app is registered for " + info.mime)]
    }

    function fileItem(path, name, dir) {
        const info = { mime: "", apps: null }
        const where = root.tilde(path.slice(0, path.lastIndexOf("/")))
        return Schema.make({
            id: "file:" + path,
            title: name + (dir ? "/" : ""),
            value: where,
            glyph: dir ? "/" : "≡",
            summary: root.tilde(path),
            probe: root.probe(path, info),
            actions: [
                {
                    id: "open", label: "open", key: "o", remember: true,
                    description: dir ? "open the folder in the file manager" : "open with the default app",
                    run: ctx => {
                        Quickshell.execDetached(["xdg-open", path])
                        ctx.done("opening " + name + "…")
                    }
                },
                {
                    id: "with", label: "open with", key: "w",
                    description: "choose one of the apps registered for its type",
                    menu: () => root.openWithMenu(path, info)
                },
                {
                    id: "reveal", label: "reveal in folder", key: "r",
                    description: "show it selected in the file manager",
                    run: ctx => {
                        Quickshell.execDetached(["sh", "-c",
                            "gdbus call --session --dest org.freedesktop.FileManager1 --object-path /org/freedesktop/FileManager1 " +
                            "--method org.freedesktop.FileManager1.ShowItems \"['file://$1']\" '' >/dev/null 2>&1 || xdg-open \"$(dirname \"$1\")\"",
                            "sh", path])
                        ctx.done("revealing " + name + "…")
                    }
                },
                {
                    id: "copy", label: "copy path", key: "c",
                    description: path,
                    run: ctx => {
                        Quickshell.execDetached(["wl-copy", "--", path])
                        ctx.done("copied " + root.tilde(path))
                    }
                }
            ]
        })
    }
}
