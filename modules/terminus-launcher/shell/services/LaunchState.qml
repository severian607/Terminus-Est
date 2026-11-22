pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io
import qs.modules.common
import "../modules/common/functions/Frecency.js" as Frecency

// What the launcher remembers, under <state dir> (~/.local/state/terminus-launcher):
//
//   launch-state.json    launch log per item id and query -> pick memory (format in Frecency.js)
//   launcher-slots.json  pinned slots, {"1": "<item id>", ...}; an old bare desktop id reads as "app:<id>"
//
// Both are written atomically (FileView.atomicWrites: temp file + rename). On first run launch-state.json is
// seeded from the old app-usage.json scores.
Singleton {
    id: root

    // Plain object, mutated in place; `rev` tells bindings it changed.
    property var state: Frecency.empty()
    property int rev: 0

    // Digit -> item id.
    property var slots: ({})

    function frecency(id) {
        root.rev
        return Frecency.frecency(root.state, id, Date.now())
    }

    function lastUsed(id) {
        root.rev
        return Frecency.lastUsed(root.state, id)
    }

    function pickBoost(query, id) {
        root.rev
        return Frecency.pickBoost(root.state, query, id)
    }

    // The state was mutated: write it out and tell the bindings.
    function saveState() {
        stateFile.setText(JSON.stringify(root.state))
        root.rev++
    }

    function record(id, query) {
        Frecency.record(root.state, id, query, Date.now())
        root.saveState()
    }

    // Launches in the log (it keeps the last 40).
    function launchCount(id) {
        root.rev
        const log = root.state.launches[id]
        return log ? log.length : 0
    }

    // [{ expr, result, t }], newest first.
    function calcs() {
        root.rev
        return root.state.calcs || []
    }

    function recordCalc(expr, result) {
        Frecency.recordCalc(root.state, expr, result, Date.now())
        root.saveState()
    }

    function slotOf(itemId) {
        for (const d in root.slots) {
            if (root.slots[d] === itemId)
                return d
        }
        return ""
    }

    // n = 1..9 pins, 0 unpins. An item holds one slot at most.
    function pin(n, itemId) {
        const next = Object.assign({}, root.slots)
        for (const d in next) {
            if (next[d] === itemId)
                delete next[d]
        }
        if (n >= 1 && n <= 9)
            next[String(n)] = itemId
        root.slots = next
        slotFile.setText(JSON.stringify(next) + "\n")
    }

    FileView {
        id: stateFile
        path: Directories.state + "/launch-state.json"
        atomicWrites: true
        blockLoading: true
        printErrors: false
        onLoaded: {
            const s = Frecency.parse(text())
            if (s) {
                root.state = s
                root.rev++
            } else {
                root.needLegacy = true
            }
        }
        onLoadFailed: root.needLegacy = true
    }

    // Only read when launch-state.json is missing or unreadable.
    property bool needLegacy: false

    FileView {
        path: root.needLegacy ? Directories.state + "/app-usage.json" : ""
        printErrors: false
        blockLoading: true
        onLoaded: if (root.needLegacy) root.seed(Frecency.importUsage(text()))
        onLoadFailed: if (root.needLegacy) root.seed(Frecency.empty())
    }

    function seed(s) {
        root.state = s
        root.saveState()
    }

    FileView {
        id: slotFile
        path: Directories.state + "/launcher-slots.json"
        atomicWrites: true
        blockLoading: true
        printErrors: false
        onLoaded: {
            const out = ({})
            try {
                const j = JSON.parse(text())
                for (let d = 1; d <= 9; d++) {
                    const id = j[String(d)]
                    if (typeof id === "string" && id.length > 0)
                        out[String(d)] = id.indexOf(":") < 0 ? "app:" + id : id
                }
            } catch (e) {
            }
            root.slots = out
        }
        onLoadFailed: root.slots = ({})
    }
}
