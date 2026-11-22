import QtQuick
import Quickshell
import Quickshell.Io
import qs.services
import qs.modules.launcher
import "../modules/launcher/Item.js" as Schema

// Computus: qalc as the calculator. "=" in front, or a query that plainly is arithmetic ("2+2*3", "(3+4)/2",
// "sqrt(16)*pi") or a unit conversion ("10 km to mi", "100 usd to eur"). The result is the top row; Enter copies it
// (wl-copy) and keeps it in the recent calculations shown in the detail pane. "=" alone lists those recents.
//
// qalc runs once per query (debounced by the router); a newer query kills the running one. Unit results are not
// auto-converted into mixed units ("6.21 mi", not "6 mi + 376 yd + ...").
Provider {
    id: root

    mode: "computus"
    prefix: "="
    global: true
    slow: true
    missing: root.hasQalc ? "" : "qalc is not installed (pacman -S libqalculate)"

    property bool hasQalc: true

    Process {
        running: true
        command: ["sh", "-c", "command -v qalc"]
        onExited: code => root.hasQalc = code === 0
    }

    // Arithmetic: digits with at least one operator, or a known function call; nothing but numbers, operators,
    // brackets, spaces and the function/constant names. A conversion: a number with a unit, "to", a unit.
    readonly property var funcs: ["sqrt", "cbrt", "sin", "cos", "tan", "asin", "acos", "atan", "ln", "log", "log10", "log2",
        "exp", "abs", "round", "floor", "ceil", "pi", "e", "mod", "rem"]

    function looksLikeMath(t) {
        const s = t.trim().toLowerCase()
        if (s === "" || !/\d/.test(s))
            return false
        if (/^[\d.\s]*\d\s*[a-z°$€£µ%/²³^·*]+(\s*[a-z°$€£µ/²³^]+)*\s+to\s+[a-z°$€£µ/²³^]+[a-z°$€£µ/²³^\s]*$/i.test(s))
            return true
        const words = s.match(/[a-z]+/g) || []
        if (words.some(w => root.funcs.indexOf(w) < 0))
            return false
        if (!/^[\d.\s+\-*/^%()!,a-z]+$/.test(s))
            return false
        return /[+\-*/^%!]/.test(s.replace(/^\s*-/, "")) || words.length > 0
    }

    function cancel() {
        root.cancelLane("qalc")
    }

    function query(text, token, prefixed) {
        root.cancel()
        const expr = text.trim()
        if (expr === "") {
            root.results(token, prefixed ? root.recentItems() : [])
            return
        }
        if (!prefixed && !root.looksLikeMath(expr)) {
            root.results(token, [])
            return
        }
        root.launch("qalc", ["qalc", "-t", "-s", "autoconversion 0", expr], out => root.answer(token, expr, out.trim()))
    }

    function answer(token, expr, out) {
        const lines = out.split("\n").map(l => l.trim()).filter(l => l !== "")
        const result = lines.length > 0 ? lines[lines.length - 1] : ""
        // qalc echoes input it could not make sense of; that is no answer.
        if (result === "" || result.replace(/\s/g, "") === expr.replace(/\s/g, "")) {
            root.results(token, [Schema.notice("computus:none", "no result for " + expr, lines.slice(0, -1).join(" ") || "qalc could not evaluate it")])
            return
        }
        root.results(token, [root.resultItem(expr, result, lines.slice(0, -1).join(" "))])
    }

    function copyAction(expr, result) {
        return {
            id: "copy",
            label: "copy",
            key: "c",
            description: "put " + result + " on the clipboard",
            run: ctx => {
                Quickshell.execDetached(["wl-copy", "--", result])
                LaunchState.recordCalc(expr, result)
                ctx.done("copied " + result)
            }
        }
    }

    function recentDetail() {
        return LaunchState.calcs().slice(0, 6).map(c => ({ label: "", text: c.expr + " = " + c.result }))
    }

    function resultItem(expr, result, note) {
        return Schema.make({
            id: "calc:" + expr,
            title: result,
            glyph: "=",
            summary: expr + (note !== "" ? " · " + note : ""),
            list: root.recentDetail(),
            actions: [root.copyAction(expr, result)]
        })
    }

    function recentItems() {
        const out = LaunchState.calcs().map(c => Schema.make({
            id: "calc:" + c.expr,
            title: c.result,
            value: Schema.ago(c.t, Date.now()),
            glyph: "=",
            summary: c.expr,
            actions: [root.copyAction(c.expr, c.result)]
        }))
        return out.length > 0 ? out : [Schema.notice("computus:empty", "no calculations yet", "type an expression after =")]
    }
}
