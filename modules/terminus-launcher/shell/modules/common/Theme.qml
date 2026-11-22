pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io

// The launcher's colours and font, read from <config dir>/theme.json (written by terminus-settings).
// A missing, empty or invalid file, or a key that is not a colour, keeps the built-in value for that key.
//
//   {"background": "#0b0b0b", "foreground": "#ececec", "muted": "#8c8c8c", "outline": "#262626",
//    "accent": "#86d993", "accentDim": "#5a8f62", "alert": "#e8a07a", "uiFont": "MonaspiceNe Nerd Font"}
// `warning` (dangerous rows) is the file's "warning" key when the theme provides one, otherwise its "alert".
//
//   qs -p <shell.qml> ipc call theme reload        re-read the file now (the file watcher can miss a replaced link)
Singleton {
    id: root

    readonly property var fallbacks: ({
        background: "#0b0b0b",
        foreground: "#ececec",
        muted: "#8c8c8c",
        outline: "#262626",
        accent: "#86d993",
        accentDim: "#5a8f62",
        alert: "#e8a07a",
        uiFont: "MonaspiceNe Nerd Font"
    })

    property color background: fallbacks.background
    property color foreground: fallbacks.foreground
    property color muted: fallbacks.muted
    property color outline: fallbacks.outline
    property color accent: fallbacks.accent
    property color accentDim: fallbacks.accentDim
    property color alert: fallbacks.alert
    property color warning: fallbacks.alert
    property string uiFont: fallbacks.uiFont

    // The file's colour for `key`, else `fallback` (default: the built-in value for that key).
    function colorOf(j, key, fallback) {
        const v = j[key]
        return (typeof v === "string" && /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(v)) ? v : (fallback !== undefined ? fallback : root.fallbacks[key])
    }

    function parse(text) {
        let j = {}
        try {
            const p = JSON.parse(text)
            if (p !== null && typeof p === "object" && !Array.isArray(p))
                j = p
        } catch (e) {
        }
        root.background = root.colorOf(j, "background")
        root.foreground = root.colorOf(j, "foreground")
        root.muted = root.colorOf(j, "muted")
        root.outline = root.colorOf(j, "outline")
        root.accent = root.colorOf(j, "accent")
        root.accentDim = root.colorOf(j, "accentDim")
        root.alert = root.colorOf(j, "alert")
        root.warning = root.colorOf(j, "warning", root.alert)
        root.uiFont = (typeof j.uiFont === "string" && j.uiFont.trim() !== "") ? j.uiFont : root.fallbacks.uiFont
    }

    function reload() {
        file.reload()
    }

    FileView {
        id: file
        path: Directories.config + "/theme.json"
        watchChanges: true
        printErrors: false
        onFileChanged: reload()
        onLoaded: root.parse(text())
        onLoadFailed: root.parse("")
    }

    IpcHandler {
        target: "theme"

        function reload(): void {
            root.reload()
        }
    }
}
