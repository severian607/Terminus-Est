import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland
import qs.services
import qs.modules.common
import qs.modules.launcher
import "../modules/common/functions/Fuzzy.js" as Fuzzy
import "../modules/launcher/Item.js" as Schema

// Index: installed apps and open windows, merged. One row per app: ● running (with its workspaces), ○ not. Enter
// focuses a running app's most recently used window, Shift+Enter opens a new one (the entry's "new window" action
// when it declares one, otherwise the app again). A window gets a row of its own when no desktop entry claims it,
// or when the query matches its title better than its app's name.
//
// Ranking with a query: the fuzzy score (Fuzzy.js) of the name and generic name, substring hits in keywords and
// (from 3 characters) the description, plus frecency (launch log, 7-day half-life) and a strong boost when this
// exact query picked the item before (LaunchState). Scattered-letter matches of apps never launched are only listed
// when nothing matches more closely; a match starting at the name's first letter ("ff" -> firefox) counts as close.
// With nothing typed: pinned slots, then recents, then running apps, then A to Z.
//
// Actions (Tab): focus, new window, move to workspace, pin, kill.
Provider {
    id: root

    mode: "index"
    global: true
    total: root.appSource.entries.length

    required property var appSource

    // The item for a pinned id ("app:<desktop id>"), or null.
    function resolve(id) {
        if (!id.startsWith("app:"))
            return null
        const e = root.appSource.entries.find(x => x.id === id.slice(4))
        if (!e)
            return null
        return root.appItem(e, root.windows().filter(w => w.appId === e.id), 0)
    }

    // The window list Quickshell keeps from Hyprland's events is current; refreshing it fills in each window's pid and
    // focus history (lastIpcObject). Asked after the first answer, so the first frame never waits on it.
    function opened() {
        root.appSource.sync()
        Qt.callLater(() => {
            Hyprland.refreshToplevels()
            staleTimer.restart()
        })
    }

    Connections {
        target: Hyprland.toplevels

        function onValuesChanged() {
            staleTimer.restart()
        }
    }

    Timer {
        id: staleTimer
        interval: 60
        onTriggered: root.stale()
    }

    // ===== windows =====

    function addressOf(t) {
        const a = String(t.address || "")
        return a === "" ? "" : (a.startsWith("0x") ? a : "0x" + a)
    }

    // [{ address, title, ws, pid, cls, appId, recency }] for every Hyprland window; appId is the desktop id or "".
    function windows() {
        const out = []
        const list = Hyprland.toplevels.values
        for (let i = 0; i < list.length; i++) {
            const t = list[i]
            const ipc = t.lastIpcObject || {}
            const cls = (t.wayland && t.wayland.appId) || ipc.class || ""
            const entry = cls !== "" ? DesktopEntries.heuristicLookup(cls) : null
            out.push({
                address: root.addressOf(t),
                title: t.title || "",
                ws: t.workspace ? String(t.workspace.name) : (ipc.workspace ? String(ipc.workspace.name) : "?"),
                pid: typeof ipc.pid === "number" ? ipc.pid : 0,
                cls: cls,
                appId: entry ? entry.id : "",
                recency: typeof ipc.focusHistoryID === "number" ? ipc.focusHistoryID : 1000 + i
            })
        }
        return out
    }

    function wsList(wins) {
        const seen = []
        for (const w of wins) {
            if (seen.indexOf(w.ws) < 0)
                seen.push(w.ws)
        }
        return seen
    }

    function pidsOf(wins) {
        const out = []
        for (const w of wins) {
            if (w.pid > 0 && out.indexOf(w.pid) < 0)
                out.push(w.pid)
        }
        return out
    }

    // ===== dispatch =====

    // Succeeds only when hyprctl prints exactly "ok": a failed dispatch prints a warning and still exits 0.
    readonly property string dispatchScript: "out=$(hyprctl dispatch \"$1\") || exit 1; [ \"$out\" = ok ] || exit 1"
    property var onFocusFailed: null

    Process {
        id: focusProc
        onExited: code => {
            const f = root.onFocusFailed
            root.onFocusFailed = null
            if (code !== 0 && f)
                f()
        }
    }

    function focusWindow(w, fallback) {
        root.onFocusFailed = fallback
        focusProc.exec(["sh", "-c", root.dispatchScript, "sh", "hl.dsp.focus({ window = \"address:" + w.address + "\" })"])
    }

    function moveWindow(w, ws) {
        Quickshell.execDetached(["sh", "-c", root.dispatchScript, "sh",
            "hl.dsp.window.move({ workspace = \"" + ws + "\", follow = false, window = \"address:" + w.address + "\" })"])
    }

    // ===== actions =====

    function newAction(e) {
        const native = root.appSource.newWindowAction(e.app)
        return {
            id: "new",
            label: "new window",
            key: "n",
            shift: true,
            remember: true,
            description: native ? "the entry's own new-window action" : "start " + e.nameL + " again",
            run: ctx => {
                if (native)
                    root.appSource.runAction(e.app, native)
                else
                    root.appSource.launch(e.app)
                ctx.done("new " + e.nameL + " window…")
            }
        }
    }

    function launchAction(e) {
        return {
            id: "launch",
            label: "launch",
            key: "l",
            remember: true,
            description: "start " + e.nameL,
            run: ctx => {
                root.appSource.launch(e.app)
                ctx.done("launching " + e.nameL + "…")
            }
        }
    }

    function focusAction(w, label, fallback) {
        return {
            id: "focus",
            label: "focus",
            key: "f",
            remember: true,
            description: "go to its window on ws " + w.ws,
            run: ctx => {
                root.focusWindow(w, fallback)
                ctx.done("focus " + label + "…")
            }
        }
    }

    function moveAction(w, label) {
        return {
            id: "move",
            label: "move to workspace",
            key: "m",
            description: "send its window on ws " + w.ws + " elsewhere; you stay where you are",
            menu: () => {
                const out = []
                for (let n = 1; n <= 10; n++) {
                    const ws = String(n)
                    out.push(Schema.make({
                        id: "ws:" + ws,
                        title: "ws " + ws,
                        value: ws === w.ws ? "here now" : "",
                        glyph: String(n % 10),
                        key: String(n % 10),
                        summary: "move " + label + " to workspace " + ws,
                        actions: [{
                            id: "move-" + ws,
                            label: "move",
                            run: ctx => {
                                root.moveWindow(w, ws)
                                ctx.done(label + " › ws " + ws)
                            }
                        }],
                        inert: ws === w.ws
                    }))
                }
                return out
            }
        }
    }

    function pinAction(itemId, label) {
        return {
            id: "pin",
            label: "pin to slot",
            key: "p",
            description: "alt+digit runs a slot from anywhere in the launcher",
            menu: () => {
                const out = []
                const own = LaunchState.slotOf(itemId)
                for (let n = 1; n <= 9; n++) {
                    const d = String(n)
                    const holder = LaunchState.slots[d] || ""
                    const held = holder === "" ? null : root.appSource.entries.find(x => "app:" + x.id === holder)
                    const title = holder === "" ? "empty" : (held ? held.nameL : holder)
                    out.push(Schema.make({
                        id: "slot:" + d,
                        title: "slot " + d,
                        value: title,
                        glyph: d,
                        key: d,
                        summary: holder === "" || holder === itemId ? "pin " + label + " here" : "replace " + title + " with " + label,
                        actions: [{
                            id: "pin-" + d,
                            label: "pin",
                            run: ctx => {
                                LaunchState.pin(n, itemId)
                                ctx.back()
                                ctx.say(label + " › slot " + d)
                            }
                        }]
                    }))
                }
                if (own !== "")
                    out.push(Schema.make({
                        id: "slot:0",
                        title: "unpin",
                        value: "slot " + own,
                        glyph: "0",
                        key: "0",
                        summary: "free slot " + own,
                        actions: [{
                            id: "unpin",
                            label: "unpin",
                            run: ctx => {
                                LaunchState.pin(0, itemId)
                                ctx.back()
                                ctx.say("unpinned " + label)
                            }
                        }]
                    }))
                return out
            }
        }
    }

    // SIGTERM to every process behind the windows: the app can still save and clean up.
    function killAction(pids, label) {
        return {
            id: "kill",
            label: "kill",
            key: "k",
            danger: true,
            confirm: true,
            description: "SIGTERM to " + (pids.length === 1 ? "pid " + pids[0] : pids.length + " processes (" + pids.join(", ") + ")"),
            run: ctx => {
                Quickshell.execDetached(["kill", "-TERM"].concat(pids.map(String)))
                ctx.done("killed " + label)
            }
        }
    }

    // ===== memory, asked for the selected row only =====

    // One process per probe, so a cancelled probe's late output can never reach a newer one.
    function probeMemory(pids) {
        return done => {
            root.launch("memory", ["sh", Directories.script("probe/memory.sh")].concat(pids.map(String)), text => {
                const n = parseInt(text)
                done([{ label: "memory", text: isFinite(n) && n > 0 ? Schema.bytes(n) : "unknown" }])
            })
        }
    }

    // ===== items =====

    // Only rows with something to say: windows only while running, the slot only when pinned (the pin key is in the
    // hint row). A closed app shows how often it was launched and the desktop actions it declares.
    function appDetail(e, id, running, windows, slot) {
        const rows = [{ label: "state", text: running ? "running" : "not running" }]
        const last = LaunchState.lastUsed(id)
        if (running) {
            rows.push({ label: "windows", text: String(windows) })
            if (last > 0)
                rows.push({ label: "last used", text: Schema.ago(last, Date.now()) })
        } else {
            const n = LaunchState.launchCount(id)
            rows.push({ label: "launched", text: n === 0 ? "never" : (n >= 40 ? "40+ times" : n === 1 ? "once" : n + " times") + ", last " + Schema.ago(last, Date.now()) })
            const acts = (e.app.actions || []).map(a => (a.name || a.id || "").toLowerCase()).filter(s => s !== "")
            if (acts.length > 0)
                rows.push({ label: "actions", text: acts.join(", ") })
        }
        if (slot !== "")
            rows.push({ label: "slot", text: "alt+" + slot })
        return rows
    }

    function appItem(e, wins, maxF) {
        const running = wins.length > 0
        const sorted = wins.slice().sort((a, b) => a.recency - b.recency)
        const ws = root.wsList(sorted)
        const id = "app:" + e.id
        const f = LaunchState.frecency(id)
        const slot = LaunchState.slotOf(id)
        const pids = root.pidsOf(sorted)
        const summary = [e.generic, e.comment].filter(s => s !== "" && s.toLowerCase() !== e.nameL).join(" · ")
        const actions = running
            ? [root.focusAction(sorted[0], e.nameL, () => root.appSource.launch(e.app)), root.newAction(e),
               root.moveAction(sorted[0], e.nameL), root.pinAction(id, e.nameL)]
            : [root.launchAction(e), root.newAction(e), root.pinAction(id, e.nameL)]
        if (running && pids.length > 0)
            actions.push(root.killAction(pids, e.nameL))
        return Schema.make({
            id: id,
            title: e.nameL,
            value: running ? "ws " + ws.join(", ") : "",
            glyph: running ? "●" : "○",
            meter: maxF > 0 ? f / maxF : 0,
            slot: slot,
            summary: summary,
            detail: root.appDetail(e, id, running, wins.length, slot),
            list: sorted.map(w => ({ label: "ws " + w.ws, text: w.title !== "" ? w.title : w.cls })),
            probe: pids.length > 0 ? root.probeMemory(pids) : null,
            actions: actions,
            pinnable: true
        })
    }

    function windowItem(w) {
        const name = w.title !== "" ? w.title : w.cls
        const label = name.toLowerCase()
        // A window's address dies with it, so focusing one is not worth remembering.
        const focus = root.focusAction(w, label, null)
        focus.remember = false
        const actions = [focus, root.moveAction(w, label)]
        if (w.pid > 0)
            actions.push(root.killAction([w.pid], label))
        return Schema.make({
            id: "win:" + w.address,
            title: name,
            value: "ws " + w.ws,
            glyph: "●",
            summary: w.cls !== "" ? w.cls + " · no app entry claims it" : "",
            detail: [
                { label: "state", text: "window" },
                { label: "workspace", text: w.ws },
                { label: "class", text: w.cls !== "" ? w.cls : "unknown" }
            ],
            probe: w.pid > 0 ? root.probeMemory([w.pid]) : null,
            actions: actions
        })
    }

    // `nameScore` is Fuzzy.score(q, e.nameL), which the caller needs as well.
    function scoreEntry(e, q, nameScore) {
        let s = nameScore
        if (s >= 900)
            return s
        s = Math.max(s, Math.round(Fuzzy.score(q, e.genericL) * 0.75))
        // Keywords and descriptions are long: only substring hits count there, never initials or scattered letters.
        s = Math.max(s, Math.round(Fuzzy.substring(q, e.keywordsL) * 0.6))
        if (q.length >= 3)
            s = Math.max(s, Math.round(Fuzzy.substring(q, e.commentL) * 0.4))
        return s
    }

    function query(text, token) {
        const q = text.trim().toLowerCase().replace(/\s+/g, " ")
        const entries = root.appSource.entries
        const wins = root.windows()
        const byApp = ({})
        const orphans = []
        for (const w of wins) {
            if (w.appId === "")
                orphans.push(w)
            else
                (byApp[w.appId] = byApp[w.appId] || []).push(w)
        }
        let maxF = 0
        const fs = ({})
        for (const e of entries) {
            const f = LaunchState.frecency("app:" + e.id)
            fs[e.id] = f
            if (f > maxF)
                maxF = f
        }

        const out = []
        if (q === "") {
            const taken = ({})
            const byId = ({})
            for (const e of entries)
                byId[e.id] = e
            const push = e => {
                if (!e || taken[e.id])
                    return
                taken[e.id] = true
                out.push(root.appItem(e, byApp[e.id] || [], maxF))
            }
            for (const d of Object.keys(LaunchState.slots).sort()) {
                const id = LaunchState.slots[d]
                if (id.startsWith("app:"))
                    push(byId[id.slice(4)])
            }
            entries.filter(e => fs[e.id] > 0).sort((a, b) => fs[b.id] - fs[a.id]).slice(0, 12).forEach(e => push(e))
            entries.filter(e => byApp[e.id]).forEach(e => push(e))
            for (const w of orphans)
                out.push(root.windowItem(w))
            entries.forEach(e => push(e))
            root.results(token, out)
            return
        }

        // base: the match quality alone (Fuzzy.js tiers); 500 and up is a substring, initials or better.
        const pickBoost = LaunchState.pickBoosts(q)
        const scored = []
        for (const e of entries) {
            const nameScore = Fuzzy.score(q, e.nameL)
            const base = root.scoreEntry(e, q, nameScore)
            if (base <= 0)
                continue
            const f = fs[e.id]
            const pick = pickBoost("app:" + e.id)
            // A subsequence starting at the name's first letter ("ff" -> firefox) ranks between a word-start hit and
            // an in-word hit, so it beats "libreoffice" for "ff".
            const sub = e.nameL.charAt(0) === q.charAt(0) ? nameScore : 0
            const rank = sub > 0 && sub < 500 ? Math.max(base, 650 + Math.round((sub - 100) / 10)) : base
            // Close: a substring, initials or better; a first-letter subsequence; or an app with any launch history,
            // which is never hidden.
            const close = rank >= 500 || LaunchState.lastUsed("app:" + e.id) > 0
            scored.push({
                close: close,
                s: rank + 150 * f / (f + 3) + 450 * pick,
                item: () => root.appItem(e, byApp[e.id] || [], maxF)
            })
        }
        for (const w of wins) {
            const own = w.appId !== "" ? entries.find(e => e.id === w.appId) : null
            // Titles are long prose too: substring hits only. A window no app claims also matches on its class.
            const ts = Math.max(Fuzzy.substring(q, w.title.toLowerCase()), own ? 0 : Fuzzy.score(q, w.cls.toLowerCase()))
            if (ts <= 0)
                continue
            if (own && root.scoreEntry(own, q, Fuzzy.score(q, own.nameL)) >= ts)
                continue
            const pick = pickBoost("win:" + w.address)
            scored.push({
                close: ts >= 500 || pick > 0,
                s: ts * 0.9 + 450 * pick,
                item: () => root.windowItem(w)
            })
        }
        // Scattered-letter matches of never-launched apps only show when nothing matches more closely.
        const close = scored.filter(x => x.close)
        const shown = close.length > 0 ? close : scored
        shown.sort((a, b) => b.s - a.s)
        root.results(token, shown.slice(0, 60).map(x => x.item()))
    }
}
