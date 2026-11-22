import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland
import Quickshell.Wayland
import qs.services
import qs.modules.common
import "Item.js" as Schema

// The launcher: an old instrument's CRT in a riveted casing, centred low on the focused monitor.
//
// It renders items (Item.js) and nothing else; where they come from is the providers' business (Router.qml).
// Panel grammar as the rest of the system: "[ launch ]" header with a rule and a mode/count slot, two numbered
// sections (¹results, ²<selected item>), one "key action" hint row.
//
//   font      Terminus (the Consoleet Terminus build: one font per native size, its outlines the bitmap's pixels),
//             at the native size closest to 17 design px; rows are that size plus even leading, and the scanline
//             period divides the row height, so every row is cut the same way
//   material  the text layer is drawn flat in three phosphor brightnesses derived from the theme accent (glow,
//             normal, dim) and goes through ONE shader (shaders/crt.frag): warm glass, bloom on the glow tier only,
//             scanlines, vignette, uneven ageing, warm-up
//   open      ~300 ms warm-up: a line opens to full height, the tube brightens unevenly, the text settles
//   close     Esc: collapse to a line, then off. The query is cleared once it is off.
//   levels    Tab opens the selected row's actions as the same screen one level deeper ("firefox › actions"); an
//             action with a menu (move to workspace, pin) opens another. Esc goes back one level before it closes.
//   keys      typing always types in the list; in an action menu a row's key (f, n, 3...) runs it. Up/Down,
//             PgUp/PgDn, Ctrl+Home/End move. Enter runs the row's first action, Shift+Enter its shift action.
//             Ctrl+1-9 pins the row to a slot (Ctrl+0 unpins), Alt+1-9 runs a slot. Ctrl+U clears the query.
//             Dangerous actions that ask (kill) need a second Enter; Esc or moving cancels.
//   IPC       qs -p <shell.qml> ipc call applauncher toggle | open | close | search <text>
//             qs -p <shell.qml> ipc call launcherslots pin <digit> <query> | slots
Scope {
    id: root

    required property var router

    // ===== phosphor (all derived from the theme) =====
    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1)
    }
    function luma(c) {
        return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b
    }

    // Warm near-black: the theme background, nudged toward the accent and toward red-orange.
    readonly property color ground: {
        const b = Theme.background
        const a = Theme.accent
        return Qt.rgba(Math.min(1, b.r * 0.92 + a.r * 0.035 + 0.014), Math.min(1, b.g * 0.92 + a.g * 0.035 + 0.007), b.b * 0.92 + a.b * 0.035, 1)
    }
    readonly property color cGlow: Theme.accent
    readonly property color cNormal: mix(Theme.accent, ground, 0.14)
    readonly property color cDim: mix(Theme.accent, ground, 0.56)
    readonly property color cRule: mix(Theme.accent, ground, 0.74)
    // The section boxes: one step under the rules, so the casing stays the strongest outline.
    readonly property color cBox: mix(Theme.accent, ground, 0.84)
    readonly property color cWarn: Theme.warning
    // The casing: a shade off the glass, with dim engraving.
    readonly property color casing: mix(ground, Theme.accentDim, 0.07)
    readonly property color casingEdge: mix(ground, Theme.accentDim, 0.45)

    // ===== limits =====
    readonly property int maxQueryLength: 128
    readonly property int wheelNotch: 120          // angleDelta units per mouse-wheel click
    readonly property int promptCells: 70          // the query shows its last 69 characters behind a "…" beyond this

    // ===== geometry, in px =====
    property var targetScreen: null
    readonly property real u: ShellScale.forScreen(root.targetScreen)

    // Terminus is only crisp at its native sizes: take the one closest to the design size (ties go up).
    readonly property var nativeSizes: [12, 14, 16, 18, 20, 22, 24, 28, 32]
    function nativeSize(px) {
        let best = nativeSizes[0]
        for (const s of nativeSizes) {
            if (Math.abs(s - px) <= Math.abs(best - px))
                best = s
        }
        return best
    }
    readonly property int fontPx: nativeSize(17 * u)
    readonly property int smallPx: nativeSize(11 * u)
    readonly property string fontFamily: "Consoleet Terminus-" + fontPx
    readonly property string plateFamily: "Consoleet Terminus-" + smallPx
    readonly property int cw: fontPx / 2                // Terminus cells are half as wide as they are tall
    // The row: the glyph cell plus even leading above and below, so the text sits on whole pixels.
    readonly property int rowPx: fontPx + 2 * Math.round(fontPx * 0.14)
    // Scanline period: the row height divided into whole periods of about 3 px.
    readonly property real g: rowPx / Math.max(1, Math.round(rowPx / 3))
    readonly property int visibleRows: 10
    readonly property int screenW: Math.round(720 * u / cw) * cw
    // Two cells in from the glass's sides, so no text sits in the vignette band.
    readonly property int pad: 2 * cw
    readonly property int listW: Math.round(screenW * 0.56 / cw) * cw

    // Every row top is a whole number of scanline periods from the screen's top. The list box holds one more row
    // than it shows items: padding, or "↓ N more".
    readonly property real headerY: 6 * g
    readonly property real promptY: headerY + rowPx + 2 * g
    // The mode strip: every mode with its prefix, the active one bright.
    readonly property real stripY: promptY + rowPx
    readonly property real boxY: stripY + rowPx + 3 * g
    readonly property real titleY: boxY + 2 * g
    readonly property real listY: titleY + rowPx
    readonly property real boxH: listY + (visibleRows + 1) * rowPx + 2 * g - boxY
    readonly property real hintY: boxY + boxH + 3 * g
    readonly property int screenH: Math.round(hintY + rowPx + 6 * g)

    readonly property int ruleGap: Math.round(3 * u)
    readonly property int inset: Math.round(10 * u)
    readonly property int plateH: Math.round(20 * u)
    readonly property int plateGap: Math.round(6 * u)
    readonly property int casingW: screenW + 2 * inset
    readonly property int casingH: 2 * inset + 2 * plateH + 2 * plateGap + screenH
    readonly property real lowered: 0.3        // 0 = centred, 1 = on the bottom edge

    // ===== state (what changes; the sections below read and change it) =====
    property bool open: false
    property bool shown: false

    // The warm-up, fed to the shader.
    property real power: 0
    property real bright: 0
    property real settle: 0
    property real fade: 1

    // The tube's clock for its live effects (LauncherConfig.motion); runs only while the window exists.
    property real tubeTime: 0
    NumberAnimation on tubeTime {
        running: root.shown && LauncherConfig.motion
        from: 0
        to: 3600
        duration: 3600 * 1000
        loops: Animation.Infinite
    }

    // The action menus opened on top of the result list: [{ crumb, items, owner, retSelected, retScroll }].
    property var stack: []
    readonly property var level: root.stack.length > 0 ? root.stack[root.stack.length - 1] : null
    readonly property var items: root.level ? root.level.items : root.router.items
    property int selected: 0
    property int scroll: 0
    readonly property var current: root.items[root.selected]

    // Type-on: when the rows change they appear top to bottom over 60 ms. `typeOn` counts rows shown.
    property string rowsSig: ""
    property real typeOn: root.visibleRows

    // The prompt's block cursor blinks; typing shows it at once.
    property bool cursorOn: true

    // The afterglow: where the selection just was, fading.
    property int ghostRow: -1

    // The selected row's slow details (Item.probe), asked for after a pause on the row.
    property var probed: []
    property int probeSeq: 0

    // Running an action: `busy` from its done() until the launcher closes; `message` replaces the hint row.
    property bool busy: false
    property string message: ""
    // The confirming action waiting for its second Enter, as "<item id>#<action id>" (a var property does not keep
    // the identity of the action object, so it is matched by key).
    property string pending: ""

    // ===== what the frame says =====
    // "[ index · 4/212 ]" while typing, "[ index · 212 ]" on an empty prompt, the breadcrumb in a menu.
    readonly property string slotText: {
        if (root.level)
            return root.level.crumb
        const mode = LauncherConfig.label(root.router.mode)
        const matches = root.router.items.filter(it => !it.inert).length
        if (root.router.query === "")
            return mode + " · " + (root.router.total > 0 ? root.router.total : matches)
        return mode + " · " + matches + (root.router.total > 0 ? "/" + root.router.total : "")
    }

    readonly property string detailLabel: {
        if (root.level)
            return root.level.owner.title
        return root.current ? root.current.title : "detail"
    }

    // The mode strip's entries, in a fixed order: [{ mode, prefix, available }]. Index has no prefix; a provider
    // whose tool or bar command is missing is listed but unavailable.
    readonly property var modeOrder: ["index", "computus", "archivum", "imperium", "memoria"]
    readonly property var modes: {
        const out = [{ mode: "index", prefix: "", available: true }]
        for (const p of root.router.providers) {
            if (p.prefix !== "")
                out.push({ mode: p.mode, prefix: p.prefix, available: p.missing === "" })
        }
        return out.sort((a, b) => root.modeOrder.indexOf(a.mode) - root.modeOrder.indexOf(b.mode))
    }

    // [{ key, action }] for the hint row, as many as fit: `p` is the priority (lower is dropped first).
    readonly property var hints: {
        const it = root.current
        const all = []
        if (root.level) {
            if (it && !it.inert && it.actions.length > 0)
                all.push({ key: "enter", action: it.actions[0].menu ? "open" : it.title, p: 9 })
            all.push({ key: "esc", action: "back", p: 8 })
        } else {
            if (it && !it.inert && it.actions.length > 0) {
                all.push({ key: "enter", action: it.actions[0].label, p: 9 })
                const s = it.actions.find(x => x.shift)
                if (s && s !== it.actions[0])
                    all.push({ key: "shift+enter", action: s.label, p: 6 })
                all.push({ key: "tab", action: "actions", p: 7 })
                if (it.pinnable)
                    all.push({ key: "ctrl+1-9", action: "pin", p: 4 })
            }
            if (root.router.query === "")
                all.push({ key: "alt+1-9", action: "slot", p: 5 })
            else
                all.push({ key: "ctrl+u", action: "clear", p: 3 })
            // The prefixes have their own strip under the prompt.
            all.push({ key: "esc", action: "close", p: 8 })
        }

        const cells = Math.floor((root.screenW - 2 * root.pad) / root.cw)
        const width = list => list.reduce((n, h) => n + h.key.length + 1 + h.action.length, 0) + 2 * Math.max(0, list.length - 1)
        let out = all
        while (out.length > 1 && width(out) > cells) {
            const low = out.reduce((m, h) => (h.p < m.p ? h : m), out[0])
            out = out.filter(h => h !== low)
        }
        return out
    }

    // ===== open / close =====

    // Terminus-Bar watches this file to glow its launcher chip while the launcher is up.
    function publishState() {
        Quickshell.execDetached(["sh", "-c", "echo " + (root.open ? "open" : "closed") + " > \"${XDG_RUNTIME_DIR:-/tmp}/terminus-launcher-state\""])
    }
    onOpenChanged: root.publishState()
    Component.onCompleted: {
        root.publishState()
        // Touch the singleton so the launch log and slots load at startup, not inside the first open.
        LaunchState.rev
        Hyprland.refreshToplevels()
    }

    function focusedScreen() {
        const name = Hyprland.focusedMonitor ? Hyprland.focusedMonitor.name : ""
        for (const s of Quickshell.screens) {
            if (s.name === name)
                return s
        }
        return Quickshell.screens.length > 0 ? Quickshell.screens[0] : null
    }

    function toggle() {
        if (root.open)
            root.close()
        else
            root.openNow("")
    }

    function openNow(text) {
        if (root.open) {
            if (text !== "") {
                root.stack = []
                root.setQuery(text)
            }
            return
        }
        closeAnim.stop()
        closeSoon.stop()
        root.targetScreen = root.focusedScreen()
        root.busy = false
        root.pending = ""
        root.message = ""
        root.stack = []
        root.router.opened()
        root.setQuery(text)
        // Without the warm-up the tube is simply on.
        const warm = LauncherConfig.warmup
        root.power = warm ? 0 : 1
        root.bright = warm ? 0 : 1
        root.settle = warm ? 0 : 1
        root.fade = 1
        root.open = true
        root.shown = true
        if (warm)
            openAnim.restart()
    }

    function close() {
        if (!root.open)
            return
        root.open = false
        closeSoon.stop()
        openAnim.stop()
        if (LauncherConfig.warmup)
            closeAnim.restart()
        else
            root.switchedOff()
    }

    ParallelAnimation {
        id: openAnim

        NumberAnimation { target: root; property: "power"; from: 0; to: 1; duration: 130; easing.type: Easing.OutCubic }
        SequentialAnimation {
            PauseAnimation { duration: 80 }
            NumberAnimation { target: root; property: "bright"; from: 0; to: 1; duration: 160; easing.type: Easing.InOutQuad }
        }
        SequentialAnimation {
            PauseAnimation { duration: 170 }
            NumberAnimation { target: root; property: "settle"; from: 0; to: 1; duration: 130; easing.type: Easing.OutQuad }
        }
    }

    SequentialAnimation {
        id: closeAnim

        NumberAnimation { target: root; property: "power"; to: 0; duration: 120; easing.type: Easing.InCubic }
        NumberAnimation { target: root; property: "fade"; to: 0; duration: 70 }
        onFinished: root.switchedOff()
    }

    // Off: the window goes and the screen is reset for the next open.
    function switchedOff() {
        if (root.open)
            return
        root.shown = false
        root.stack = []
        root.pending = ""
        root.message = ""
        root.setQuery("")
    }

    // ===== levels and selection =====

    function push(crumb, items, owner) {
        root.stack = root.stack.concat([{ crumb: crumb, items: items, owner: owner, retSelected: root.selected, retScroll: root.scroll }])
        root.selected = 0
        root.scroll = 0
        root.pending = ""
        root.message = ""
    }

    function pop() {
        const top = root.level
        if (!top)
            return
        root.stack = root.stack.slice(0, -1)
        root.selected = top.retSelected
        root.scroll = top.retScroll
        root.pending = ""
        root.message = ""
        // Back on the list: actions in the menus (a pin) may have changed what it shows.
        if (root.stack.length === 0)
            root.router.refresh()
        root.keepVisible()
    }

    // Tab: the selected row's actions; inside a menu, the selected action's own menu.
    function openActions() {
        const it = root.current
        if (!it || it.inert || it.actions.length === 0)
            return
        if (!root.level) {
            root.push(it.title + " › actions", Schema.actionItems(it), it)
            return
        }
        const a = it.actions[0]
        if (a.menu)
            root.push(root.level.owner.title + " › " + a.label, a.menu(), root.level.owner)
    }

    function setQuery(text) {
        root.selected = 0
        root.scroll = 0
        root.router.update(text)
    }

    onItemsChanged: {
        if (root.selected >= root.items.length)
            root.selected = Math.max(0, root.items.length - 1)
        root.keepVisible()
        // Only a real change of rows types on again; a refresh that brings back the same rows does not.
        const sig = root.items.map(it => it.id).join("\n")
        if (sig !== root.rowsSig) {
            root.rowsSig = sig
            if (LauncherConfig.typeOn)
                typeOnAnim.restart()
        }
    }

    NumberAnimation {
        id: typeOnAnim
        target: root
        property: "typeOn"
        from: 0
        to: root.visibleRows
        duration: 60
    }

    Timer {
        id: cursorTimer
        interval: 530
        repeat: true
        running: root.open && !root.level && LauncherConfig.cursorBlink
        onRunningChanged: if (!running) root.cursorOn = true
        onTriggered: root.cursorOn = !root.cursorOn
    }
    Connections {
        target: root.router

        function onQueryChanged() {
            root.cursorOn = true
            cursorTimer.restart()
        }
    }

    function keepVisible() {
        if (root.selected < root.scroll)
            root.scroll = root.selected
        else if (root.selected >= root.scroll + root.visibleRows)
            root.scroll = root.selected - root.visibleRows + 1
        root.scroll = Math.max(0, Math.min(root.scroll, Math.max(0, root.items.length - root.visibleRows)))
    }

    function select(index) {
        if (index === root.selected)
            return
        root.ghostRow = root.selected
        root.selected = index
        root.keepVisible()
        if (LauncherConfig.afterglow)
            ghostAnim.restart()
    }

    function move(delta) {
        if (root.items.length > 0)
            root.select(Math.max(0, Math.min(root.items.length - 1, root.selected + delta)))
    }

    // ===== the selected row's slow details =====

    onCurrentChanged: {
        root.probed = []
        root.probeSeq++
        probeTimer.restart()
        if (root.pending !== "") {
            root.pending = ""
            root.message = ""
        }
    }

    Timer {
        id: probeTimer
        interval: 90
        onTriggered: {
            const it = root.current
            if (!it || !it.probe || !root.open)
                return
            const seq = root.probeSeq
            it.probe(rows => {
                if (seq === root.probeSeq)
                    root.probed = rows
            })
        }
    }

    // ===== running actions =====

    Timer {
        id: messageReset
        interval: 1600
        onTriggered: if (!root.busy && root.pending === "") root.message = ""
    }

    // The launcher stays up a beat after acting, so the message can be read.
    Timer {
        id: closeSoon
        interval: 420
        onTriggered: root.close()
    }

    readonly property var ctx: ({
        done: msg => {
            root.busy = true
            root.message = msg || ""
            closeSoon.restart()
        },
        say: msg => {
            root.message = msg
            messageReset.restart()
        },
        back: () => root.pop()
    })

    // `owner`: the item the action belongs to (the list row, also behind every menu level).
    function runAction(owner, a) {
        if (!a || root.busy)
            return
        if (a.menu) {
            root.push(owner.title + " › " + a.label, a.menu(), owner)
            return
        }
        const key = owner.id + "#" + a.id
        if (a.confirm && root.pending !== key) {
            root.pending = key
            root.message = a.confirmText || ("enter again to " + a.label + " " + owner.title + " · esc cancels")
            return
        }
        root.pending = ""
        if (a.remember)
            LaunchState.record(owner.id, root.router.text)
        a.run(root.ctx)
    }

    function enter(shift) {
        const it = root.current
        if (!it || it.inert || it.actions.length === 0)
            return
        if (root.level) {
            root.runAction(root.level.owner, it.actions[0])
            return
        }
        const a = shift ? (it.actions.find(x => x.shift) || it.actions[0]) : it.actions[0]
        root.runAction(it, a)
    }

    // In an action menu: the row whose key was typed.
    function pressKey(ch) {
        const i = root.items.findIndex(it => it.key === ch && !it.inert)
        if (i < 0)
            return false
        root.select(i)
        root.enter(false)
        return true
    }

    function pinSelected(n) {
        const it = root.current
        if (!it || !it.pinnable) {
            root.ctx.say("this row cannot be pinned")
            return
        }
        LaunchState.pin(n, it.id)
        root.ctx.say(n === 0 ? "unpinned " + it.title : it.title + " › slot " + n)
        root.router.refresh()
    }

    function runSlot(n) {
        const id = LaunchState.slots[String(n)]
        const it = id ? root.router.resolve(id) : null
        if (!it) {
            root.ctx.say("slot " + n + " is empty")
            return
        }
        root.runAction(it, it.actions[0])
    }

    // ===== keys =====

    function digitOf(key) {
        return key >= Qt.Key_0 && key <= Qt.Key_9 ? key - Qt.Key_0 : -1
    }

    function escapeKey() {
        if (root.pending !== "") {
            root.pending = ""
            root.message = ""
        } else if (root.level) {
            root.pop()
        } else {
            root.close()
        }
    }

    function onKey(event) {
        if (!root.open)
            return
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        const meta = (event.modifiers & Qt.MetaModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const d = root.digitOf(event.key)
        event.accepted = true
        if (event.key === Qt.Key_Escape) {
            root.escapeKey()
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (!event.isAutoRepeat)
                root.enter(shift)
        } else if (event.key === Qt.Key_Tab) {
            if (!event.isAutoRepeat && !root.busy)
                root.openActions()
        } else if (event.key === Qt.Key_Down) {
            root.move(1)
        } else if (event.key === Qt.Key_Up) {
            root.move(-1)
        } else if (event.key === Qt.Key_PageDown) {
            root.move(root.visibleRows)
        } else if (event.key === Qt.Key_PageUp) {
            root.move(-root.visibleRows)
        } else if (event.key === Qt.Key_Home && ctrl) {
            root.move(-root.items.length)
        } else if (event.key === Qt.Key_End && ctrl) {
            root.move(root.items.length)
        } else if (root.busy) {
            return
        } else if (root.level) {
            // An action menu: keys run rows, nothing types.
            if (ctrl || alt || meta || event.isAutoRepeat || event.text.length !== 1 || !root.pressKey(event.text.toLowerCase()))
                event.accepted = event.key === Qt.Key_Backspace
        } else if (event.key === Qt.Key_Backspace) {
            if (ctrl)
                root.setQuery("")
            else if (root.router.query !== "")
                root.setQuery(root.router.query.slice(0, -1))
        } else if (ctrl && event.key === Qt.Key_U) {
            root.setQuery("")
        } else if (ctrl && d >= 0) {
            if (!event.isAutoRepeat)
                root.pinSelected(d)
        } else if (alt && d >= 1) {
            if (!event.isAutoRepeat)
                root.runSlot(d)
        } else if (ctrl || alt || meta) {
            event.accepted = false
        } else if (event.text.length === 1 && event.text.charCodeAt(0) >= 32) {
            if (root.router.query.length < root.maxQueryLength)
                root.setQuery(root.router.query + event.text)
        } else {
            event.accepted = false
        }
    }

    // ===== IPC =====
    IpcHandler {
        target: "applauncher"

        function toggle(): void {
            root.toggle()
        }

        function open(): void {
            root.openNow("")
        }

        function close(): void {
            root.close()
        }

        // Open with this query typed (e.g. "=" for the calculator).
        function search(text: string): void {
            root.openNow(text)
        }
    }

    // Slots: pin a match to a digit, list the pins.
    IpcHandler {
        target: "launcherslots"

        // Pin the best match for `query` to slot `digit` (0 clears it).
        function pin(digit: int, query: string): string {
            if (digit < 0 || digit > 9)
                return "digit must be 0-9"
            const it = root.router.best(query)
            if (!it)
                return "nothing pinnable matches " + query
            LaunchState.pin(digit, it.id)
            return (digit > 0 ? "slot " + digit + " = " : "unpinned ") + it.title
        }

        function slots(): string {
            const out = []
            for (let d = 1; d <= 9; d++) {
                const id = LaunchState.slots[String(d)]
                if (id)
                    out.push(d + " " + id)
            }
            return out.join("\n")
        }
    }

    // ===== components =====
    component Cell: Text {
        font.family: root.fontFamily
        font.pixelSize: root.fontPx
        font.hintingPreference: Font.PreferNoHinting
        font.kerning: false
        color: root.cNormal
        height: root.rowPx
        verticalAlignment: Text.AlignVCenter
        renderType: Text.NativeRendering
        textFormat: Text.PlainText
        maximumLineCount: 1
    }

    component Rule: Rectangle {
        height: 1
        color: root.cRule
    }

    // A 1 px outline with a clear inside; the colour is the section boxes' unless set.
    component Outline: Rectangle {
        color: "transparent"
        border.width: 1
        border.color: root.cBox
    }

    // One line of the detail pane: a dim label and its text, which starts at `labelCols` cells (at 0 when unlabelled).
    component DetailLine: Item {
        property string label
        property string text
        property int labelCols
        height: root.rowPx

        Cell {
            text: parent.label
            color: root.cDim
        }
        Cell {
            x: parent.label === "" ? 0 : parent.labelCols * root.cw
            width: parent.width - x
            elide: Text.ElideRight
            text: parent.text
        }
    }

    // One visible row of the result list: view position `index` shows item `root.scroll + index`.
    component ResultRow: Item {
        id: row
        required property int index
        readonly property int meterCells: 5
        readonly property int at: root.scroll + index
        readonly property var it: root.items[at]
        readonly property bool sel: it !== undefined && at === root.selected
        readonly property color ink: sel ? root.ground : (it && it.danger ? root.cWarn : (it && it.inert ? root.cDim : root.cNormal))
        readonly property color inkDim: sel ? root.ground : root.cDim
        readonly property var meter: it && it.meter >= 0 ? Schema.meterText(it.meter, meterCells) : null

        x: root.pad + root.cw
        y: root.listY + index * root.rowPx
        width: root.listW - 2 * root.cw
        height: root.rowPx
        visible: it !== undefined && index < root.typeOn

        Rectangle {
            anchors.fill: parent
            color: row.it && row.it.danger ? root.cWarn : root.cNormal
            visible: row.sel
        }
        Cell {
            x: 0
            text: row.it && row.it.slot ? row.it.slot : " "
            color: row.inkDim
        }
        Cell {
            x: 2 * root.cw
            text: row.it ? row.it.glyph : ""
            color: row.sel ? root.ground : (row.it && row.it.glyph === "●" ? root.cNormal : root.cDim)
        }
        Cell {
            x: 4 * root.cw
            width: valueText.x - x - root.cw
            elide: Text.ElideRight
            text: row.it ? row.it.title : ""
            color: row.ink
        }
        Cell {
            id: valueText
            // At most 45% of the row; long values (paths) keep their end.
            width: Math.min(implicitWidth, Math.floor(row.width * 0.45 / root.cw) * root.cw)
            elide: Text.ElideLeft
            x: (row.meter ? meterOn.x - root.cw : row.width) - width
            text: row.it ? row.it.value : ""
            color: row.inkDim
        }
        Cell {
            id: meterOn
            x: row.width - row.meterCells * root.cw
            text: row.meter ? row.meter.on : ""
            color: row.sel ? root.ground : root.cNormal
            visible: row.meter !== null
        }
        Cell {
            x: meterOn.x + meterOn.width
            text: row.meter ? row.meter.off : ""
            color: row.inkDim
            visible: row.meter !== null
        }
        MouseArea {
            anchors.fill: parent
            onClicked: {
                root.select(row.at)
                root.enter(false)
            }
        }
    }

    // A row of the detail pane is one line of label + text, or (with `image`) a picture six rows tall, tinted to
    // the phosphor like everything else on the tube.
    component DetailRow: Item {
        required property var modelData
        width: parent.width
        height: modelData.image ? 6 * root.rowPx : root.rowPx

        DetailLine {
            visible: !modelData.image
            width: parent.width
            label: modelData.label
            text: modelData.text
            labelCols: 10
        }
        Image {
            id: pic
            visible: false
            anchors.fill: parent
            anchors.topMargin: root.rowPx / 2
            source: modelData.image ? "file://" + modelData.image : ""
            fillMode: Image.PreserveAspectFit
            horizontalAlignment: Image.AlignLeft
            asynchronous: true
            cache: false
            sourceSize.height: height
        }
        MultiEffect {
            anchors.fill: pic
            source: pic
            visible: !!modelData.image
            colorization: 1.0
            colorizationColor: root.cNormal
        }
    }

    // Engraved lettering on the casing plates.
    component Engraving: Text {
        anchors.verticalCenter: parent.verticalCenter
        color: root.cDim
        font.family: root.plateFamily
        font.pixelSize: root.smallPx
        font.hintingPreference: Font.PreferNoHinting
        font.letterSpacing: root.smallPx / 2
        renderType: Text.NativeRendering
    }

    // A riveted strip of the casing with engraved lettering.
    component Plate: Item {
        id: plate
        property string leftText: ""
        property string rightText: ""
        property string centreText: ""
        width: root.screenW
        height: root.plateH

        Rectangle {
            anchors.fill: parent
            color: Qt.lighter(root.casing, 1.25)
            border.width: 1
            border.color: root.casingEdge
        }
        Repeater {
            model: 2

            Rectangle {
                required property int index
                readonly property real d: Math.round(5 * root.u)
                width: d
                height: d
                radius: d / 2
                x: index === 0 ? Math.round(6 * root.u) : plate.width - Math.round(6 * root.u) - d
                anchors.verticalCenter: parent.verticalCenter
                color: root.casingEdge
                border.width: 1
                border.color: root.mix(root.casingEdge, root.ground, 0.5)
            }
        }
        Engraving {
            x: Math.round(18 * root.u)
            text: plate.leftText
            visible: plate.leftText !== ""
        }
        Engraving {
            x: plate.width - Math.round(18 * root.u) - contentWidth + font.letterSpacing
            text: plate.rightText
            visible: plate.rightText !== ""
        }
        Engraving {
            x: Math.round((plate.width - contentWidth + font.letterSpacing) / 2)
            text: plate.centreText
            visible: plate.centreText !== ""
        }
    }

    // ===== view =====
    PanelWindow {
        id: win

        visible: root.shown
        screen: root.targetScreen
        color: "transparent"
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusionMode: ExclusionMode.Ignore
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "terminus-launcher"
        WlrLayershell.keyboardFocus: root.open ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

        FocusScope {
            anchors.fill: parent
            focus: true
            Keys.onPressed: event => root.onKey(event)

            // Anywhere outside the casing puts it away.
            MouseArea {
                anchors.fill: parent
                onClicked: root.close()
            }

            WheelHandler {
                property real travel: 0

                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    travel += event.angleDelta.y
                    const rows = Math.trunc(travel / root.wheelNotch)
                    if (rows !== 0) {
                        travel -= rows * root.wheelNotch
                        root.move(-rows)
                    }
                }
            }

            // ----- the casing -----
            Item {
                id: casing
                width: root.casingW
                height: root.casingH
                x: Math.round((parent.width - width) / 2)
                y: Math.round((parent.height - height) / 2 * (1 + root.lowered))
                opacity: root.fade

                MouseArea {
                    anchors.fill: parent
                }

                Rectangle {
                    anchors.fill: parent
                    color: root.casing
                }

                // The double rule.
                Outline {
                    anchors.fill: parent
                    visible: LauncherConfig.frame
                    border.color: root.casingEdge
                }
                Outline {
                    anchors.fill: parent
                    anchors.margins: root.ruleGap
                    visible: LauncherConfig.frame
                    border.color: root.casingEdge
                }

                Plate {
                    x: root.inset
                    y: root.inset
                    leftText: "☼ terminus · est"
                    rightText: "apparatus i"
                }

                // ----- the tube -----
                Item {
                    id: tube
                    x: root.inset
                    y: root.inset + root.plateH + root.plateGap
                    width: root.screenW
                    height: root.screenH

                    // The text layer, flat; only the shader below draws it.
                    Item {
                        id: content
                        width: root.screenW
                        height: root.screenH

                        // header: [ launch ] ──── [ index · 4/212 ]
                        Cell {
                            id: headLeft
                            x: root.pad
                            y: root.headerY
                            text: "[ launch ]"
                            color: root.cGlow
                        }
                        Cell {
                            id: headRight
                            x: root.screenW - root.pad - width
                            y: root.headerY
                            text: "[ " + root.slotText + " ]"
                            color: root.cGlow
                        }
                        Rule {
                            x: headLeft.x + headLeft.width + root.cw
                            width: headRight.x - root.cw - x
                            y: root.headerY + root.rowPx / 2
                        }

                        // prompt: › query█ (dim, without the cursor, while an action menu is open)
                        Row {
                            x: root.pad
                            y: root.promptY
                            height: root.rowPx

                            Cell {
                                text: "› "
                                color: root.level ? root.cDim : root.cGlow
                            }
                            Cell {
                                text: root.router.query.length > root.promptCells ? "…" + root.router.query.slice(1 - root.promptCells) : root.router.query
                                color: root.level ? root.cDim : root.cGlow
                            }
                            Rectangle {
                                y: Math.round((root.rowPx - root.fontPx) / 2)
                                width: root.cw
                                height: root.fontPx
                                color: root.cGlow
                                visible: !root.level && root.cursorOn
                            }
                        }

                        // mode strip: index  = computus  / archivum  : imperium  @ memoria
                        Row {
                            x: root.pad + 2 * root.cw
                            y: root.stripY
                            height: root.rowPx
                            spacing: 2 * root.cw

                            Repeater {
                                model: root.modes

                                Cell {
                                    required property var modelData
                                    text: (modelData.prefix !== "" ? modelData.prefix + " " : "") + LauncherConfig.label(modelData.mode)
                                    // Active: bright. Available: dim. Unavailable (its tool or bar command is missing):
                                    // dimmer still and struck through.
                                    color: !modelData.available ? root.cBox : (modelData.mode === root.router.mode ? root.cGlow : root.cDim)
                                    font.strikeout: !modelData.available
                                }
                            }
                        }

                        // ¹results
                        Outline {
                            x: root.pad
                            y: root.boxY
                            width: root.listW
                            height: root.boxH
                        }
                        Cell {
                            id: resultsTitle
                            x: root.pad + root.cw
                            y: root.titleY
                            text: root.level ? "¹actions" : "¹results"
                            color: root.cGlow
                        }
                        Rule {
                            x: resultsTitle.x + resultsTitle.width + root.cw
                            width: root.pad + root.listW - root.cw - x
                            y: root.titleY + root.rowPx / 2
                        }

                        // the afterglow under the rows
                        Rectangle {
                            id: ghost
                            readonly property int vis: root.ghostRow - root.scroll
                            x: root.pad + root.cw
                            width: root.listW - 2 * root.cw
                            y: root.listY + vis * root.rowPx
                            height: root.rowPx
                            color: root.cNormal
                            opacity: 0
                            visible: vis >= 0 && vis < root.visibleRows

                            NumberAnimation on opacity {
                                id: ghostAnim
                                running: false
                                from: 0.38
                                to: 0
                                duration: 170
                                easing.type: Easing.OutQuad
                            }
                        }

                        Repeater {
                            model: root.visibleRows

                            ResultRow {}
                        }

                        Cell {
                            x: root.pad + 5 * root.cw
                            y: root.listY
                            visible: root.items.length === 0
                            text: "nothing matches"
                            color: root.cDim
                        }
                        // The padding row: how many rows wait below the view.
                        Cell {
                            readonly property int below: root.items.length - root.scroll - root.visibleRows
                            x: root.pad + 3 * root.cw
                            y: root.listY + root.visibleRows * root.rowPx
                            visible: below > 0
                            text: "↓ " + below + " more"
                            color: root.cDim
                        }

                        // ²<item>
                        Outline {
                            id: detailBox
                            x: root.pad + root.listW + root.cw
                            y: root.boxY
                            width: root.screenW - root.pad - x
                            height: root.boxH
                        }
                        Cell {
                            id: detailTitle
                            x: detailBox.x + root.cw
                            y: root.titleY
                            width: Math.min(implicitWidth, detailBox.width - 4 * root.cw)
                            elide: Text.ElideRight
                            text: "²" + root.detailLabel
                            color: root.current && root.current.danger ? root.cWarn : root.cGlow
                        }
                        Rule {
                            x: detailTitle.x + detailTitle.width + root.cw
                            width: Math.max(0, detailBox.x + detailBox.width - root.cw - x)
                            y: root.titleY + root.rowPx / 2
                        }

                        Column {
                            id: detailCol
                            x: detailBox.x + root.cw
                            y: root.listY
                            width: detailBox.width - 2 * root.cw
                            visible: root.current !== undefined
                            // Long details (a file preview) stop at the box's last row.
                            height: (root.visibleRows + 1) * root.rowPx
                            clip: true

                            readonly property var rows: root.current ? root.current.detail.concat(root.probed) : []
                            readonly property var list: root.current && root.current.list ? root.current.list : []
                            // The summary takes one row, or two when it does not fit on one.
                            readonly property int summaryRows: summary.text === "" ? 0 : (summaryMetrics.advanceWidth > width ? 2 : 1)
                            // Rows left for the list once summary, spacers and detail rows are placed.
                            readonly property int room: root.visibleRows + 1 - summaryRows - (summaryRows > 0 ? 1 : 0) - rows.length - 1
                            readonly property int shownList: list.length <= room ? list.length : Math.max(0, room - 1)

                            TextMetrics {
                                id: summaryMetrics
                                font: summary.font
                                text: summary.text
                            }
                            Cell {
                                id: summary
                                width: parent.width
                                height: detailCol.summaryRows * root.rowPx
                                maximumLineCount: 2
                                wrapMode: Text.Wrap
                                elide: Text.ElideRight
                                lineHeightMode: Text.FixedHeight
                                lineHeight: root.rowPx
                                verticalAlignment: Text.AlignTop
                                visible: text !== ""
                                text: root.current ? (root.current.summary || "") : ""
                                color: root.cDim
                            }
                            Item {
                                width: 1
                                height: root.rowPx
                                visible: summary.visible
                            }
                            Repeater {
                                model: detailCol.rows

                                DetailRow {}
                            }
                            Item {
                                width: 1
                                height: root.rowPx
                                visible: detailCol.shownList > 0
                            }
                            Repeater {
                                model: detailCol.list.slice(0, detailCol.shownList)

                                DetailLine {
                                    required property var modelData
                                    width: parent.width
                                    label: modelData.label
                                    text: modelData.text
                                    labelCols: 6
                                }
                            }
                            Cell {
                                visible: detailCol.list.length > detailCol.shownList && detailCol.shownList > 0
                                text: "+" + (detailCol.list.length - detailCol.shownList) + " more"
                                color: root.cDim
                            }
                        }

                        // hint row: key action  key action …, or what the launcher is doing
                        Row {
                            x: root.pad
                            y: root.hintY
                            height: root.rowPx
                            spacing: 2 * root.cw
                            visible: root.message === ""

                            Repeater {
                                model: root.hints

                                Row {
                                    required property var modelData
                                    spacing: root.cw

                                    Cell {
                                        text: modelData.key
                                        color: root.cGlow
                                    }
                                    Cell {
                                        text: modelData.action
                                        color: root.cDim
                                    }
                                }
                            }
                        }
                        Cell {
                            x: root.pad
                            y: root.hintY
                            visible: root.message !== ""
                            text: root.message
                            color: root.pending !== "" ? root.cWarn : root.cGlow
                        }
                    }

                    ShaderEffectSource {
                        id: contentSource
                        sourceItem: content
                        hideSource: true
                        live: true
                        visible: false
                    }

                    ShaderEffect {
                        anchors.fill: parent
                        property variant source: contentSource
                        property color ground: root.ground
                        property color phosphor: root.cGlow
                        property size size: Qt.size(width, height)
                        property point texel: Qt.point(1 / Math.max(1, width), 1 / Math.max(1, height))
                        property real period: root.g
                        property real scanDepth: 0.34
                        property real bloomFrom: root.luma(root.cGlow) * 0.92
                        property real bloom: 1.25
                        property real power: root.power
                        property real bright: root.bright
                        property real settle: root.settle
                        property real fade: 1
                        property real seed: 7.31
                        property real mask: 0.09
                        property real grain: 0.035
                        property real corner: Math.round(18 * root.u)
                        // The vignette band, in px from the sides and from the top/bottom: inside the text margins.
                        property point vigBand: Qt.point(root.pad - 4, 6 * root.g - 4)
                        property real glass: 1
                        property real time: root.tubeTime
                        property real motion: LauncherConfig.motion ? 1 : 0
                        fragmentShader: Qt.resolvedUrl("../../shaders/crt.frag.qsb")
                    }

                    // A thin dark bezel where the glass meets the casing.
                    Outline {
                        anchors.fill: parent
                        anchors.margins: -1
                        border.color: root.mix(root.ground, "#000000", 0.5)
                    }
                }

                Plate {
                    x: root.inset
                    y: tube.y + tube.height + root.plateGap
                    centreText: "this is the line of division"
                }
            }
        }
    }
}
