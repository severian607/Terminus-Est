pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io
import qs.modules.common

// <config dir>/launcher.json (a link to config/terminus-launcher/launcher.json), re-read when saved:
//
//   { "frame": true,          the double-ruled frame around the casing
//     "motion": true,         the tube's live effects (phosphor grain, refresh band, brightness waver)
//     "animation": {          each of the launcher's animations, all on by default:
//       "warmup": true,         the tube opening from a line and collapsing to one (off: it simply appears/goes)
//       "typeOn": true,         rows appearing top to bottom when the results change
//       "cursorBlink": true,    the prompt's block cursor blinking (off: a steady block)
//       "afterglow": true },    the fading trace the selection leaves when it moves
//     "labels": "latin" }     mode names: "latin", "plain", or an object overriding single modes,
//                             e.g. {"index": "apps", "computus": "calc"} (unnamed modes keep the Latin name)
//
// A missing or invalid file keeps these defaults.
Singleton {
    id: root

    readonly property var latin: ({
        index: "index",
        computus: "computus",
        archivum: "archivum",
        imperium: "imperium",
        memoria: "memoria",
        actions: "actions"
    })
    readonly property var plain: ({
        index: "apps",
        computus: "calculator",
        archivum: "files",
        imperium: "system",
        memoria: "clipboard",
        actions: "actions"
    })

    property bool frame: true
    property bool motion: true
    property bool warmup: true
    property bool typeOn: true
    property bool cursorBlink: true
    property bool afterglow: true
    property var labels: latin

    function label(mode) {
        return root.labels[mode] || mode
    }

    function parse(text) {
        let j = {}
        try {
            const p = JSON.parse(text)
            if (p && typeof p === "object" && !Array.isArray(p))
                j = p
        } catch (e) {
        }
        root.frame = j.frame !== false
        root.motion = j.motion !== false
        const a = (j.animation && typeof j.animation === "object") ? j.animation : {}
        root.warmup = a.warmup !== false
        root.typeOn = a.typeOn !== false
        root.cursorBlink = a.cursorBlink !== false
        root.afterglow = a.afterglow !== false
        if (j.labels === "plain")
            root.labels = root.plain
        else if (j.labels && typeof j.labels === "object")
            root.labels = Object.assign({}, root.latin, j.labels)
        else
            root.labels = root.latin
    }

    FileView {
        path: Directories.config + "/launcher.json"
        watchChanges: true
        printErrors: false
        onFileChanged: reload()
        onLoaded: root.parse(text())
        onLoadFailed: root.parse("")
    }
}
