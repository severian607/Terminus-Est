import QtQuick
import Quickshell
import "Item.js" as Schema

// Sends the query to the providers and merges their answers into `items`, in provider order.
//
//   - A query starting with a provider's prefix goes to that provider alone, without the prefix ("=2+2" -> "2+2").
//     Otherwise every `global` provider answers (each may decide the text is not for it, e.g. computus answers only
//     arithmetic). Providers learn which it was from query()'s `prefixed` argument.
//   - Fast providers are asked at once; slow ones after `debounce` ms without typing.
//   - Each query gets a new token. Answers carrying an older token are dropped, and providers that stop being
//     asked are cancelled, so stale results never appear. A slow provider's old rows are cleared, not kept,
//     while its new answer is pending.
//   - A provider whose tool is missing answers with one notice row naming it, when its prefix was typed.
Scope {
    id: root

    property var providers: []
    property int debounce: 30

    property string query: ""
    property int token: 0
    property var items: []
    // The active mode: the prefixed provider's, or "index".
    property string mode: "index"
    property bool prefixed: false
    // What the mode could answer with at all: the prefixed provider's, or the index's (the extras that also answer
    // unprefixed, such as system commands, are not counted there).
    readonly property int total: root.active.filter(p => root.prefixed || p.mode === "index").reduce((n, p) => n + p.total, 0)

    // mode -> items, for the current token.
    property var buckets: ({})
    property var active: []
    property string text: ""

    function connectAll() {
        for (const p of root.providers) {
            const provider = p
            provider.results.connect((tok, list) => root.accept(provider, tok, list))
            provider.stale.connect(() => {
                if (root.active.indexOf(provider) >= 0)
                    root.refresh()
            })
        }
    }

    Component.onCompleted: root.connectAll()

    function opened() {
        for (const p of root.providers)
            p.opened()
    }

    function resolve(id) {
        for (const p of root.providers) {
            const it = p.resolve(id)
            if (it)
                return it
        }
        return null
    }

    // The best unprefixed match for `text` from the fast providers, answered synchronously (IPC pin).
    function best(text) {
        let found = null
        for (const p of root.providers.filter(x => x.global && !x.slow && x.missing === "")) {
            const take = (tok, list) => {
                if (tok === -1 && !found)
                    found = list.find(it => !it.inert && it.pinnable) || null
            }
            p.results.connect(take)
            p.query(text, -1)
            p.results.disconnect(take)
            if (found)
                break
        }
        return found
    }

    function route(q) {
        for (const p of root.providers) {
            if (p.prefix !== "" && q.startsWith(p.prefix))
                return { list: [p], text: q.slice(p.prefix.length), mode: p.mode, prefixed: true }
        }
        return { list: root.providers.filter(p => p.global), text: q, mode: "index", prefixed: false }
    }

    // Re-run the current query (state the providers read changed, e.g. a pin).
    function refresh() {
        root.update(root.query)
    }

    function update(q) {
        root.query = q
        root.token++
        const r = root.route(q)
        for (const p of root.active) {
            if (r.list.indexOf(p) < 0 || p.slow)
                p.cancel()
        }
        root.active = r.list
        if (r.prefixed)
            r.list[0].recheck()
        root.text = r.text
        root.prefixed = r.prefixed
        root.mode = r.mode
        root.buckets = ({})
        slowTimer.stop()
        let anySlow = false
        for (const p of r.list) {
            // A missing tool is named when its mode was asked for by prefix; unprefixed it stays out of the way.
            if (p.missing !== "") {
                if (r.prefixed)
                    root.buckets[p.mode] = [Schema.notice(p.mode + ":missing", "unavailable", p.missing)]
            } else if (p.slow) {
                anySlow = true
            } else {
                p.query(r.text, root.token, r.prefixed)
            }
        }
        if (anySlow)
            slowTimer.restart()
        root.merge()
    }

    Timer {
        id: slowTimer
        interval: root.debounce
        onTriggered: {
            for (const p of root.active) {
                if (p.slow && p.missing === "")
                    p.query(root.text, root.token, root.prefixed)
            }
        }
    }

    function accept(p, tok, list) {
        if (tok !== root.token || root.active.indexOf(p) < 0)
            return
        root.buckets[p.mode] = list
        root.merge()
    }

    function merge() {
        let out = []
        for (const p of root.active) {
            const b = root.buckets[p.mode]
            if (b)
                out = out.concat(b)
        }
        root.items = out
    }
}
