import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland
import Quickshell.Wayland

// Keyboard overlay: every SUPER binding drawn on a keyboard. Opened by `terminus-keys` (SUPER + H), one process per open.
//
//   layer     SUPER is always on; click SHIFT / CTRL / ALT (or hold them) to see that layer
//   keys      lit = bound on this layer; a dot = bound only on another layer
//   readout   hover or click (or press) a key to list all of its SUPER bindings
//   close     Esc, a click outside the panel, or SUPER + H again
//
// Data: bindings.py (live `hyprctl binds`). Colours: ~/.config/terminus-launcher/theme.json (written by terminus-settings).
ShellRoot {
    id: root

    // ===== theme =====
    property color bg: "#0b0b0b"
    property color fg: "#ececec"
    property color muted: "#8c8c8c"
    property color outline: "#262626"
    property color accent: "#ffb3b0"
    property color accentDim: "#a77775"
    property string uiFont: "MonaspiceNe Nerd Font"

    function themeColor(j, key, fallback) {
        const v = j[key]
        return (typeof v === "string" && /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(v)) ? v : fallback
    }

    FileView {
        path: Quickshell.env("HOME") + "/.config/terminus-launcher/theme.json"
        printErrors: false
        onLoaded: {
            let j = {}
            try {
                j = JSON.parse(text())
            } catch (e) {
            }
            root.bg = root.themeColor(j, "background", root.bg)
            root.fg = root.themeColor(j, "foreground", root.fg)
            root.muted = root.themeColor(j, "muted", root.muted)
            root.outline = root.themeColor(j, "outline", root.outline)
            root.accent = root.themeColor(j, "accent", root.accent)
            root.accentDim = root.themeColor(j, "accentDim", root.accentDim)
            if (typeof j.uiFont === "string" && j.uiFont.trim() !== "")
                root.uiFont = j.uiFont
        }
    }

    // ===== bindings =====
    property var binds: []                 // [{mods: ["SHIFT"], key: "T", desc: "..."}]

    Process {
        running: true
        command: ["python3", Qt.resolvedUrl("bindings.py").toString().replace("file://", "")]
        stdout: StdioCollector {
            onStreamFinished: {
                try {
                    root.binds = JSON.parse(text)
                } catch (e) {
                    root.binds = []
                }
            }
        }
    }

    function layerOf(b) {
        return (b.mods.indexOf("SHIFT") >= 0 ? 1 : 0) | (b.mods.indexOf("CTRL") >= 0 ? 4 : 0) | (b.mods.indexOf("ALT") >= 0 ? 8 : 0)
    }

    // key id -> [{layer, desc}] sorted by layer
    readonly property var byKey: {
        const m = {}
        for (const b of root.binds) {
            if (!m[b.key])
                m[b.key] = []
            m[b.key].push({ layer: root.layerOf(b), desc: b.desc, short: b.short })
        }
        for (const k in m)
            m[k].sort((a, c) => a.layer - c.layer)
        return m
    }

    // ===== layer =====
    property int clicked: 0                // modifiers toggled with the mouse (1 shift, 4 ctrl, 8 alt)
    property int held: 0                   // modifiers held on the real keyboard
    readonly property int layer: clicked | held

    // The bar's shortcut badges show only while ALT is down (held or toggled on screen).
    readonly property bool altDown: (layer & 8) !== 0
    onAltDownChanged: Quickshell.execDetached(["terminus-bar", "msg", "bar-hints", altDown ? "on" : "off"])

    function layerName(l) {
        let s = "SUPER"
        if (l & 1) s += " + SHIFT"
        if (l & 4) s += " + CTRL"
        if (l & 8) s += " + ALT"
        return s
    }

    function descOn(id, l) {
        const list = root.byKey[id]
        if (!list)
            return ""
        for (const e of list)
            if (e.layer === l)
                return e.short
        return ""
    }

    // ===== layout (units of one key) =====
    function k(id, label, w, code, mod) {
        return { id: id, label: label, w: w, code: code, mod: mod || 0 }
    }
    function sp(w) {
        return { sp: w }
    }

    readonly property var rows: [
        [k("ESCAPE", "ESC", 1, 9), k("F1", "F1", 1, 67), k("F2", "F2", 1, 68), k("F3", "F3", 1, 69), k("F4", "F4", 1, 70),
         k("F5", "F5", 1, 71), k("F6", "F6", 1, 72), k("F7", "F7", 1, 73), k("F8", "F8", 1, 74), k("F9", "F9", 1, 75),
         k("F10", "F10", 1, 76), k("F11", "F11", 1, 95), k("F12", "F12", 1, 96), k("PRINT", "PRT", 1, 107), k("DELETE", "DEL", 1, 119)],
        [k("GRAVE", "`", 1, 49), k("1", "1", 1, 10), k("2", "2", 1, 11), k("3", "3", 1, 12), k("4", "4", 1, 13), k("5", "5", 1, 14),
         k("6", "6", 1, 15), k("7", "7", 1, 16), k("8", "8", 1, 17), k("9", "9", 1, 18), k("0", "0", 1, 19), k("MINUS", "-", 1, 20),
         k("EQUAL", "=", 1, 21), k("BACKSPACE", "BKSP", 2, 22)],
        [k("TAB", "TAB", 1.5, 23), k("Q", "Q", 1, 24), k("W", "W", 1, 25), k("E", "E", 1, 26), k("R", "R", 1, 27), k("T", "T", 1, 28),
         k("Y", "Y", 1, 29), k("U", "U", 1, 30), k("I", "I", 1, 31), k("O", "O", 1, 32), k("P", "P", 1, 33), k("BRACKETLEFT", "[", 1, 34),
         k("BRACKETRIGHT", "]", 1, 35), k("BACKSLASH", "\\", 1.5, 51)],
        [k("CAPSLOCK", "CAPS", 1.75, 66), k("A", "A", 1, 38), k("S", "S", 1, 39), k("D", "D", 1, 40), k("F", "F", 1, 41), k("G", "G", 1, 42),
         k("H", "H", 1, 43), k("J", "J", 1, 44), k("K", "K", 1, 45), k("L", "L", 1, 46), k("SEMICOLON", ";", 1, 47),
         k("APOSTROPHE", "'", 1, 48), k("RETURN", "ENTER", 2.25, 36)],
        [k("SHIFT", "SHIFT", 2.25, 50, 1), k("Z", "Z", 1, 52), k("X", "X", 1, 53), k("C", "C", 1, 54), k("V", "V", 1, 55), k("B", "B", 1, 56),
         k("N", "N", 1, 57), k("M", "M", 1, 58), k("COMMA", ",", 1, 59), k("PERIOD", ".", 1, 60), k("SLASH", "/", 1, 61),
         k("SHIFT", "SHIFT", 2.75, 62, 1)],
        [k("CTRL", "CTRL", 1.25, 37, 4), k("SUPER", "SUPER", 1.25, 133, 64), k("ALT", "ALT", 1.25, 64, 8), k("SPACE", "SPACE", 6.25, 65),
         k("ALT", "ALT", 1.25, 108, 8), k("FN", "FN", 1.25, 0), k("MENU", "MENU", 1.25, 135), k("CTRL", "CTRL", 1.25, 105, 4)]
    ]

    readonly property real cx: 15.75       // the pointer / arrows cluster
    readonly property var cluster: [
        { id: "MOUSE_L", label: "LMB", x: 0, y: 1 }, { id: "MOUSE_R", label: "RMB", x: 1, y: 1 },
        { id: "WHEEL_UP", label: "WHL \u2191", x: 0, y: 2 }, { id: "WHEEL_DOWN", label: "WHL \u2193", x: 1, y: 2 },
        { id: "XF86AUDIOMUTE", label: "MUTE", x: 0, y: 3 }, { id: "COPILOT", label: "AI", x: 1, y: 3 },
        { id: "UP", label: "\u2191", x: 1, y: 4, code: 111 }, { id: "LEFT", label: "\u2190", x: 0, y: 5, code: 113 },
        { id: "DOWN", label: "\u2193", x: 1, y: 5, code: 116 }, { id: "RIGHT", label: "\u2192", x: 2, y: 5, code: 114 }
    ]

    readonly property var keys: {
        const out = []
        for (let r = 0; r < root.rows.length; r++) {
            let x = 0
            for (const key of root.rows[r]) {
                out.push({ id: key.id, label: key.label, x: x, y: r === 0 ? 0 : r + 0.5, w: key.w, code: key.code, mod: key.mod })
                x += key.w
            }
        }
        for (const c of root.cluster)
            out.push({ id: c.id, label: c.label, x: root.cx + c.x, y: c.y + 0.5, w: 1, code: c.code || 0, mod: 0 })
        return out
    }
    readonly property real gridW: 18.75
    readonly property real gridH: 6.5

    // bindings whose key has no cap on the board
    readonly property var unplaced: {
        const placed = {}
        for (const key of root.keys)
            placed[key.id] = true
        const seen = {}
        const out = []
        for (const b of root.binds)
            if (!placed[b.key] && !seen[b.key + b.desc]) {
                seen[b.key + b.desc] = true
                out.push(root.layerName(root.layerOf(b)) + " + " + b.key + "  " + b.desc)
            }
        return out
    }

    // ===== selection =====
    property string selected: ""
    property string hovered: ""
    readonly property string active: root.hovered !== "" ? root.hovered : root.selected

    function keyIdForScanCode(code) {
        for (const key of root.keys)
            if (key.code === code && key.mod === 0)
                return key.id
        return ""
    }

    function modOf(key) {
        if (key === Qt.Key_Shift) return 1
        if (key === Qt.Key_Control) return 4
        if (key === Qt.Key_Alt || key === Qt.Key_AltGr) return 8
        return 0
    }

    readonly property var focusedScreen: {
        const name = Hyprland.focusedMonitor ? Hyprland.focusedMonitor.name : ""
        for (const s of Quickshell.screens)
            if (s.name === name)
                return s
        return Quickshell.screens.length > 0 ? Quickshell.screens[0] : null
    }

    PanelWindow {
        id: win

        screen: root.focusedScreen
        color: "transparent"
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusionMode: ExclusionMode.Ignore
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "terminus-keys"
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

        readonly property real u: Math.max(36, Math.min(80, (width * 0.94 - 64) / root.gridW))

        FocusScope {
            anchors.fill: parent
            focus: true

            Keys.onPressed: event => {
                event.accepted = true
                if (event.key === Qt.Key_Escape) {
                    Qt.quit()
                    return
                }
                const m = root.modOf(event.key)
                if (m !== 0) {
                    root.held |= m
                    return
                }
                const id = root.keyIdForScanCode(event.nativeScanCode)
                if (id !== "") {
                    root.hovered = ""
                    root.selected = id
                }
            }
            Keys.onReleased: event => {
                const m = root.modOf(event.key)
                if (m !== 0)
                    root.held &= ~m
            }

            Rectangle {
                anchors.fill: parent
                // While ALT is down the bar and its badge strip (24 px bar + 28 px badges) are left undimmed.
                anchors.topMargin: root.altDown ? 52 : 0
                color: "#000000"
                opacity: 0.62

                MouseArea {
                    anchors.fill: parent
                    onClicked: Qt.quit()
                }
            }

            Rectangle {
                id: panel

                anchors.centerIn: parent
                width: root.gridW * win.u + 2 * 28
                height: header.height + board.height + readout.height + 4 * 20 + 28
                color: root.bg
                border.width: 1
                border.color: root.accentDim

                MouseArea {
                    anchors.fill: parent      // keeps clicks on the panel from reaching the scrim
                    onClicked: root.selected = ""
                }

                // ---- header ----
                Item {
                    id: header

                    x: 28
                    y: 20
                    width: parent.width - 56
                    height: 34

                    Text {
                        id: title
                        anchors.verticalCenter: parent.verticalCenter
                        text: "KEYBINDINGS"
                        color: root.muted
                        font.family: root.uiFont
                        font.pixelSize: 12
                        font.letterSpacing: 3
                    }
                    Text {
                        anchors.left: title.right
                        anchors.leftMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.layerName(root.layer)
                        color: root.accent
                        font.family: root.uiFont
                        font.pixelSize: 20
                        font.bold: true
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        Repeater {
                            model: [{ name: "SHIFT", bit: 1 }, { name: "CTRL", bit: 4 }, { name: "ALT", bit: 8 }]

                            Rectangle {
                                required property var modelData
                                readonly property bool on: (root.layer & modelData.bit) !== 0

                                width: 64
                                height: 26
                                color: on ? root.accent : "transparent"
                                border.width: 1
                                border.color: on ? root.accent : root.outline

                                Text {
                                    anchors.centerIn: parent
                                    text: parent.modelData.name
                                    color: parent.on ? root.bg : root.muted
                                    font.family: root.uiFont
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.clicked ^= parent.modelData.bit
                                }
                            }
                        }
                    }
                }

                // ---- keyboard ----
                Item {
                    id: board

                    x: 28
                    y: header.y + header.height + 20
                    width: root.gridW * win.u
                    height: root.gridH * win.u

                    Repeater {
                        model: root.keys

                        Rectangle {
                            id: cap

                            required property var modelData
                            readonly property string id: modelData.id
                            readonly property bool isMod: modelData.mod !== 0
                            readonly property bool isSuper: modelData.mod === 64
                            readonly property string desc: isMod ? "" : root.descOn(id, root.layer)
                            readonly property bool lit: desc !== ""
                            readonly property bool elsewhere: !isMod && !lit && root.byKey[id] !== undefined
                            readonly property bool modOn: isMod && (isSuper || (root.layer & modelData.mod) !== 0)
                            readonly property bool picked: !isMod && root.active === id
                            readonly property real pad: 2

                            x: modelData.x * win.u + pad
                            y: modelData.y * win.u + pad
                            width: modelData.w * win.u - 2 * pad
                            height: win.u - 2 * pad
                            color: modOn ? root.accent : (picked ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.34)
                                : (lit ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.14) : "transparent"))
                            border.width: 1
                            border.color: modOn || lit || picked ? root.accent : (elsewhere ? root.accentDim : root.outline)

                            Text {
                                x: 5
                                y: 3
                                text: cap.modelData.label
                                color: cap.modOn ? root.bg : (cap.lit || cap.picked ? root.accent : (cap.elsewhere ? root.fg : root.muted))
                                font.family: root.uiFont
                                font.pixelSize: Math.max(9, win.u * 0.19)
                                font.bold: cap.lit || cap.modOn
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 5
                                anchors.bottomMargin: 3
                                visible: cap.lit
                                text: cap.desc
                                color: root.fg
                                wrapMode: Text.Wrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                                lineHeight: 0.85
                                font.family: root.uiFont
                                font.pixelSize: Math.max(8, win.u * 0.135)
                            }
                            Rectangle {              // bound only on another layer
                                visible: cap.elsewhere
                                x: parent.width - 9
                                y: 5
                                width: 4
                                height: 4
                                color: root.accentDim
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: if (!cap.isMod) root.hovered = cap.id
                                onExited: if (root.hovered === cap.id) root.hovered = ""
                                onClicked: {
                                    if (cap.isMod) {
                                        if (!cap.isSuper)
                                            root.clicked ^= cap.modelData.mod
                                    } else {
                                        root.selected = root.selected === cap.id ? "" : cap.id
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- readout ----
                Item {
                    id: readout

                    x: 28
                    y: board.y + board.height + 20
                    width: parent.width - 56
                    height: 5 * 20 + 8

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: root.outline
                    }
                    Column {
                        y: 10
                        width: parent.width
                        spacing: 0

                        Text {
                            visible: root.active === ""
                            text: "Hover or press a key to see everything SUPER does with it.  Click SHIFT / CTRL / ALT, or hold them, to switch layer."
                            color: root.muted
                            font.family: root.uiFont
                            font.pixelSize: 12
                            height: 20
                        }
                        Text {
                            visible: root.active === "" && root.unplaced.length > 0
                            text: "Not on the board: " + root.unplaced.join("   \u00b7   ")
                            color: root.muted
                            font.family: root.uiFont
                            font.pixelSize: 11
                            height: 20
                            elide: Text.ElideRight
                            width: parent.width
                        }
                        Repeater {
                            model: root.active === "" ? [] : (root.byKey[root.active] || [])

                            Row {
                                required property var modelData
                                readonly property bool here: modelData.layer === root.layer

                                spacing: 16
                                height: 20

                                Text {
                                    width: 240
                                    text: root.layerName(parent.modelData.layer) + " + " + root.active
                                    color: parent.here ? root.accent : root.muted
                                    font.family: root.uiFont
                                    font.pixelSize: 12
                                    font.bold: parent.here
                                }
                                Text {
                                    text: parent.modelData.desc
                                    color: parent.here ? root.fg : root.muted
                                    font.family: root.uiFont
                                    font.pixelSize: 12
                                }
                            }
                        }
                        Text {
                            visible: root.active !== "" && root.byKey[root.active] === undefined
                            text: "SUPER + " + root.active + "  is not bound"
                            color: root.muted
                            font.family: root.uiFont
                            font.pixelSize: 12
                            height: 20
                        }
                    }
                }
            }
        }
    }
}
