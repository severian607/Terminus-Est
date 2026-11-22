import QtQuick
import Quickshell

// The shape every source has: a query goes in, items (Item.js) come out through `results`.
//
//   mode      the mode key ("index", "computus", ...); its label comes from LauncherConfig
//   prefix    the character that selects this provider alone ("=", "/", ...); "" for none
//   global    also answers queries without a prefix (merged into the default list)
//   slow      the router waits ~30 ms of quiet typing before asking (external processes)
//   missing   non-empty when the provider's tool is not installed: the router lists it as a notice row
//   total     how many things it can answer with at all (the header's "matches/total"); 0 when unknown
//
// query(text, token, prefixed): `text` has the prefix removed; `prefixed` says whether this provider's prefix was
// typed (false: the query is for every global provider). Answer with results(token, items), now or later; the router
// drops answers whose token is not the current one. cancel(): a newer query or a mode change made the running
// work useless (kill processes here).
Scope {
    id: root

    property string mode: ""
    property string prefix: ""
    property bool global: false
    property bool slow: false
    property string missing: ""
    property int total: 0

    // Lane -> the one-shot process running in it (see launch()); plain scratch, not reactive.
    readonly property var flights: ({})

    signal results(int token, var items)
    // What the provider showed for the current query changed (a window opened): the router asks again.
    signal stale()

    Component {
        id: oneShot

        OneShot {}
    }

    // Runs `command` for its stdout: done(text) gets it unless the process is cancelled first.
    function spawn(command, done) {
        return oneShot.createObject(root, { command: command, done: done, running: true })
    }

    // Runs `command` as the only process of `lane`: the one running there is silenced and stopped first.
    function launch(lane, command, done) {
        root.cancelLane(lane)
        const proc = root.spawn(command, done)
        root.flights[lane] = proc
        proc.exited.connect(() => {
            if (root.flights[lane] === proc)
                delete root.flights[lane]
        })
    }

    function cancelLane(lane) {
        const proc = root.flights[lane]
        if (!proc)
            return
        delete root.flights[lane]
        proc.done = null
        proc.running = false
    }

    function query(text, token, prefixed) {
        root.results(token, [])
    }

    function cancel() {
    }

    // Its prefix was typed: re-check whatever `missing` depends on (a tool, a bar command), so the provider becomes
    // available without a launcher restart. Emit stale() when the answer changes.
    function recheck() {
    }

    // The launcher opened: refresh anything cached.
    function opened() {
    }

    // The item with this id (a pinned slot), or null when it is not this provider's or no longer exists.
    function resolve(id) {
        return null
    }
}
