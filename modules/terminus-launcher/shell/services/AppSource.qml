import Quickshell
import Quickshell.Io
import QtQuick
import qs.modules.common

// Every installed .desktop application, via Quickshell's own DesktopEntries index (no shelling out), and launching
// them. Ranking lives in the index provider (providers/IndexProvider.qml); this file only keeps the list.
//
//   - `entries`: ONE plain object per app, built once (and again only when apps are installed / removed),
//     name-sorted, carrying the lower-cased search fields. Typing never touches the DesktopEntry objects again
//     (each read of one is a trip into C++), it just compares strings.
//   - the objects are STABLE: the same app is the same object every time.
Scope {
    id: root

    // Plain-JS scratch, not reactive (see sync()).
    readonly property var _cache: ({ sig: "" })

    // DesktopEntries.applications.values is reactive, but an app added to ~/.local/share/applications after the
    // shell started stayed missing until a reload. So the list is also re-read on purpose (`sync()`): when the
    // model says its values changed, and every time the launcher opens. `_rev` re-runs the binding when it differs.
    property int _rev: 0
    readonly property var entries: {
        root._rev
        return root.buildEntries(DesktopEntries.applications.values)
    }

    function signatureOf(list) {
        let sig = list.length + ":"
        for (let i = 0; i < list.length; i++)
            sig += (list[i].id || list[i].name || "") + "|"
        return sig
    }

    function sync() {
        const sig = root.signatureOf(DesktopEntries.applications.values)
        if (sig !== root._cache.sig)
            root._rev++
    }

    Connections {
        target: DesktopEntries.applications

        function onValuesChanged() {
            Qt.callLater(root.sync)
        }
    }

    // Desktop entries kept out of the launcher, per machine: <config dir>/hidden-apps.json, a JSON array of
    // desktop ids (no file = nothing hidden). On the desktop it hides org.gnome.Nautilus, which is named "Blaze" there
    // and whose Exec points at ~/Projects/Terminus-Est/bin/blaze, which does not exist.
    property var hiddenIds: []

    FileView {
        path: Directories.config + "/hidden-apps.json"
        blockLoading: true
        watchChanges: true
        printErrors: false
        onLoaded: {
            try {
                const j = JSON.parse(text())
                root.hiddenIds = Array.isArray(j) ? j : []
            } catch (e) {
                root.hiddenIds = []
            }
        }
        onLoadFailed: root.hiddenIds = []
        onFileChanged: reload()
    }

    function buildEntries(list) {
        root._cache.sig = root.signatureOf(list)
        const apps = list.filter(a => root.hiddenIds.indexOf(a.id) < 0)
        apps.sort((a, b) => a.name.localeCompare(b.name))
        const out = new Array(apps.length)
        for (let i = 0; i < apps.length; i++) {
            const a = apps[i]
            const name = a.name || ""
            const generic = a.genericName || ""
            out[i] = {
                id: a.id || name,
                idx: i,
                app: a,
                name: name,
                generic: generic,
                comment: a.comment || "",
                nameL: name.toLowerCase(),
                genericL: generic.toLowerCase(),
                keywordsL: (a.keywords || []).join(" ").toLowerCase()
            }
        }
        return out
    }

    // Through uwsm-app so the program gets its own systemd scope, like every other app started from this desktop,
    // and outlives this shell. `command` has the desktop field codes (%u, %F...) removed.
    function launch(app) {
        if (!app)
            return
        root.run(app.runInTerminal ? ["ghostty", "-e"].concat(app.command) : app.command, app.workingDirectory)
    }

    // The entry's "new window" desktop action (Firefox, Chromium, Zed...), or null when it declares none.
    function newWindowAction(app) {
        const list = (app && app.actions) || []
        for (const a of list) {
            if (/^new-?window$/i.test(a.id || "") || /^new window$/i.test(a.name || ""))
                return a
        }
        return null
    }

    function runAction(app, action) {
        root.run(action.command, app.workingDirectory)
    }

    function run(argv, workingDirectory) {
        Quickshell.execDetached({
            command: ["uwsm-app", "--"].concat(argv),
            workingDirectory: workingDirectory || ""
        })
    }
}
