.pragma library

// The launcher's memory: a launch log per item, which query led to which pick, and recent calculations.
//
//   { "version": 1,
//     "launches": { "<item id>": [t, t, ...] },            ms timestamps, oldest first, at most MAX_LAUNCHES
//     "picks":    { "<query>": { "<item id>": { "n": 3, "t": ms } } },
//     "calcs":    [{ "expr": "2+2", "result": "4", "t": ms }, ...] }   newest first, at most MAX_CALCS
//
// frecency(id) sums 0.5^(age / HALF_LIFE) over the log, so a launch counts 1 now, 0.5 a week ago, 0.25 two weeks ago.
// pickBoosts(query)(id) is 0..1: the share of the picks for exactly this query that went to this item, weighted up
// with repetition (one pick 0.5 of its share, three picks 0.75).

var HALF_LIFE_MS = 7 * 24 * 3600 * 1000;
var MAX_LAUNCHES = 40;
var MAX_QUERIES = 400;
var MAX_CALCS = 20;
var FORGET_HALF_LIVES = 8; // an item unused this long scores under 0.4% of a launch and is dropped

function empty() {
    return { version: 1, launches: {}, picks: {}, calcs: [] };
}

function parse(text) {
    try {
        var j = JSON.parse(text);
        if (j && typeof j === "object" && j.version === 1) {
            return {
                version: 1,
                launches: (j.launches && typeof j.launches === "object") ? j.launches : {},
                picks: (j.picks && typeof j.picks === "object") ? j.picks : {},
                calcs: Array.isArray(j.calcs) ? j.calcs : []
            };
        }
    } catch (e) {
    }
    return null;
}

function normQuery(q) {
    return (q || "").trim().toLowerCase().replace(/\s+/g, " ");
}

// Mutates and returns `state`. The same expression again moves to the top instead of repeating.
function recordCalc(state, expr, result, now) {
    var e = (expr || "").trim();
    state.calcs = (state.calcs || []).filter(function (c) { return c.expr !== e; });
    state.calcs.unshift({ expr: e, result: result, t: now });
    if (state.calcs.length > MAX_CALCS)
        state.calcs.length = MAX_CALCS;
    return state;
}

function frecency(state, id, now) {
    var log = state.launches[id];
    if (!log)
        return 0;
    var s = 0;
    for (var i = 0; i < log.length; i++)
        s += Math.pow(0.5, Math.max(0, now - log[i]) / HALF_LIFE_MS);
    return s;
}

function lastUsed(state, id) {
    var log = state.launches[id];
    return log && log.length > 0 ? log[log.length - 1] : 0;
}

// The boost function of one query: the picks for exactly this query are looked up once, then it costs a lookup per id.
function pickBoosts(state, query) {
    var p = state.picks[normQuery(query)];
    return function (id) {
        if (!p || !p[id])
            return 0;
        var total = 0;
        for (var k in p)
            total += p[k].n;
        var n = p[id].n;
        return (n / total) * (n / (n + 1));
    };
}

// Mutates and returns `state`.
function record(state, id, query, now) {
    var log = state.launches[id] || [];
    log.push(now);
    if (log.length > MAX_LAUNCHES)
        log.splice(0, log.length - MAX_LAUNCHES);
    state.launches[id] = log;
    // Ids never repeat for some items (files), so the log would otherwise only grow.
    var cutoff = now - FORGET_HALF_LIVES * HALF_LIFE_MS;
    for (var k in state.launches) {
        var l = state.launches[k];
        if (l.length === 0 || l[l.length - 1] < cutoff)
            delete state.launches[k];
    }

    var q = normQuery(query);
    if (q !== "") {
        var p = state.picks[q] || {};
        var e = p[id] || { n: 0, t: 0 };
        e.n += 1;
        e.t = now;
        p[id] = e;
        state.picks[q] = p;
        var keys = Object.keys(state.picks);
        if (keys.length > MAX_QUERIES) {
            var latest = function (k) {
                var m = 0;
                for (var i in state.picks[k])
                    m = Math.max(m, state.picks[k][i].t);
                return m;
            };
            keys.sort(function (a, b) { return latest(a) - latest(b); });
            for (var i = 0; i < keys.length - MAX_QUERIES; i++)
                delete state.picks[keys[i]];
        }
    }
    return state;
}

// The old per-app decaying scores (app-usage.json: { "<desktop id>": { "s": score, "t": last ms } }) become
// round(s) launches at their last-used time, at most 10, keyed "app:<desktop id>".
function importUsage(text) {
    var state = empty();
    try {
        var j = JSON.parse(text);
        for (var id in j) {
            var u = j[id];
            if (!u || typeof u.s !== "number" || typeof u.t !== "number")
                continue;
            var n = Math.max(1, Math.min(10, Math.round(u.s)));
            var log = [];
            for (var i = 0; i < n; i++)
                log.push(u.t);
            state.launches["app:" + id] = log;
        }
    } catch (e) {
    }
    return state;
}
