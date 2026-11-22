import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland
import qs.modules.launcher
import "../modules/common/functions/Fuzzy.js" as Fuzzy
import "../modules/launcher/Item.js" as Schema

// Imperium: session and system commands through the bar's own msg interface (terminus-bar msg ...), so the
// launcher does exactly what the dashboard does. ":" lists them all; without the prefix they answer when the query
// is a close match ("reboot", "night"), below the apps.
//
// Logout, reboot and shutdown are dangerous: drawn in the warning tone, and Enter asks once before running (the same
// second-Enter confirm as kill). Their detail pane states the effect and how many apps are open.
Provider {
    id: root

    mode: "imperium"
    prefix: ":"
    global: true
    total: root.commands.length
    missing: root.hasBar ? "" : "terminus-bar is not on PATH: system commands go through it"

    property bool hasBar: true

    // The bar's do-not-disturb state ("on", "off"; "" until known), read each time the launcher opens.
    property string dnd: ""

    Process {
        id: dndProc
        command: ["terminus-bar", "msg", "notification-dnd-status"]
        stdout: StdioCollector {
            onStreamFinished: {
                const s = text.trim()
                const v = s === "on" || s === "off" ? s : ""
                if (v !== root.dnd) {
                    root.dnd = v
                    root.stale()
                }
            }
        }
    }

    function opened() {
        if (root.hasBar && !dndProc.running)
            dndProc.running = true
    }

    Process {
        running: true
        command: ["sh", "-c", "command -v terminus-bar"]
        onExited: code => root.hasBar = code === 0
    }

    // id, title, words it also answers to, what it does, the msg arguments; danger asks first.
    readonly property var commands: [
        { id: "lock", title: "lock", words: "lock screen", effect: "locks the screen", msg: ["session", "lock"] },
        { id: "sleep", title: "sleep", words: "suspend", effect: "suspends the machine; open apps stay as they are", msg: ["session", "suspend"] },
        { id: "logout", title: "logout", words: "log out sign out exit session", effect: "ends the session and closes every app", msg: ["session", "logout"], danger: true },
        { id: "reboot", title: "reboot", words: "restart", effect: "closes every app and restarts the machine", msg: ["session", "reboot"], danger: true },
        { id: "shutdown", title: "shutdown", words: "power off poweroff halt", effect: "closes every app and powers the machine off", msg: ["session", "shutdown"], danger: true },
        { id: "nightlight", title: "night light", words: "nightlight redshift warm screen", effect: "toggles the forced night light, as the dashboard does", msg: ["nightlight-force-toggle"] },
        { id: "dnd", title: "do not disturb", words: "dnd quiet mute notifications", effect: "toggles holding notifications back, as the dashboard does", msg: ["notification-dnd-toggle"] },
        { id: "caffeine", title: "keep awake", words: "caffeine idle inhibit awake", effect: "toggles the idle inhibitor (caffeine)", msg: ["caffeine-toggle"] },
        { id: "micmute", title: "mic mute", words: "microphone mute unmute", effect: "toggles the microphone mute", msg: ["mic-mute"] },
        { id: "rescan", title: "rescan displays", words: "monitors ddc brightness", effect: "rescans displays for DDC/CI brightness control", msg: ["display-rescan"] }
    ]

    function openApps() {
        const list = Hyprland.toplevels.values
        const classes = []
        for (const t of list) {
            const c = (t.wayland && t.wayland.appId) || (t.lastIpcObject && t.lastIpcObject.class) || "?"
            if (classes.indexOf(c) < 0)
                classes.push(c)
        }
        return { windows: list.length, apps: classes.length }
    }

    function item(c) {
        const open = root.openApps()
        const detail = [{ label: "effect", text: c.effect }]
        if (c.id === "dnd" && root.dnd !== "")
            detail.push({ label: "now", text: root.dnd })
        if (c.danger)
            detail.push({ label: "open now", text: open.apps + " apps, " + open.windows + " windows" })
        detail.push({ label: "runs", text: "terminus-bar msg " + c.msg.join(" ") })
        return Schema.make({
            id: "sys:" + c.id,
            title: c.title,
            value: c.danger ? "asks first" : (c.id === "dnd" ? root.dnd : ""),
            glyph: "§",
            detail: detail,
            actions: [{
                id: "run",
                label: c.title,
                key: "r",
                danger: !!c.danger,
                confirm: !!c.danger,
                confirmText: "enter again to " + c.title + " · " + open.apps + " apps open · esc cancels",
                remember: true,
                description: c.effect,
                run: ctx => {
                    Quickshell.execDetached(["terminus-bar", "msg"].concat(c.msg))
                    ctx.done(c.title + "…")
                }
            }],
            danger: !!c.danger
        })
    }

    function query(text, token, prefixed) {
        const q = text.trim().toLowerCase().replace(/\s+/g, " ")
        if (q === "") {
            root.results(token, prefixed ? root.commands.map(c => root.item(c)) : [])
            return
        }
        const scored = []
        for (const c of root.commands) {
            const s = Math.max(Fuzzy.score(q, c.title), Math.round(Fuzzy.substring(q, c.words) * 0.9))
            // Unprefixed, only a close match answers (a prefix or word start), so "e" does not list reboot.
            if (s >= (prefixed ? 1 : 700))
                scored.push({ s: s, c: c })
        }
        scored.sort((a, b) => b.s - a.s)
        root.results(token, scored.map(x => root.item(x.c)))
    }
}
