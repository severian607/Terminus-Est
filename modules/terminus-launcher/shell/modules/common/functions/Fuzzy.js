.pragma library

// Fuzzy matching for the launcher. score(query, text) is 0 for no match, higher is better (max 1000).
// Both arguments are expected lower-cased. Tiers, best first:
//
//   1000      exact
//   900-850   prefix                       "fire"  -> "firefox"
//   800-700   substring at a word start    "code"  -> "visual studio code"
//   750-700   initials                     "vsc"   -> "visual studio code"
//   600-500   substring inside a word      "fox"   -> "firefox"
//   499-100   subsequence; word starts and contiguous runs earn more, gaps cost
//
// A query with spaces matches when every word matches; it scores the mean of the words, slightly lowered.

var BOUNDARY = " -_./:()[]";

function isStart(text, i) {
    return i === 0 || BOUNDARY.indexOf(text.charAt(i - 1)) >= 0;
}

function initialsOf(text) {
    var out = "";
    for (var i = 0; i < text.length; i++) {
        if (isStart(text, i) && BOUNDARY.indexOf(text.charAt(i)) < 0)
            out += text.charAt(i);
    }
    return out;
}

// Best subsequence alignment (dynamic programming over query x text). Returns 0 when q is not a subsequence.
function subsequence(q, text) {
    var m = q.length, n = text.length;
    if (m === 0 || m > n || m > 32)
        return 0;
    var NEG = -1e9;
    // prev[j]: best score with q[i-1] matched at text[j].
    var prev = new Array(n), cur = new Array(n);
    var j;
    for (j = 0; j < n; j++)
        prev[j] = text.charAt(j) === q.charAt(0) ? 10 + (isStart(text, j) ? 18 : 0) - Math.min(j, 12) : NEG;
    for (var i = 1; i < m; i++) {
        var c = q.charAt(i);
        var bestBefore = NEG, bestBeforeAt = -1;
        for (j = 0; j < n; j++) {
            cur[j] = NEG;
            if (j > 0 && text.charAt(j) === c) {
                var start = isStart(text, j) ? 18 : 0;
                // Contiguous with the previous match.
                if (prev[j - 1] > NEG)
                    cur[j] = prev[j - 1] + 10 + 14 + start;
                // After a gap.
                if (bestBefore > NEG) {
                    var gap = j - bestBeforeAt - 1;
                    cur[j] = Math.max(cur[j], bestBefore + 10 + start - Math.min(gap, 8));
                }
            }
            // bestBefore covers prev[0..j-1] for the next column.
            if (prev[j] > bestBefore) {
                bestBefore = prev[j];
                bestBeforeAt = j;
            }
        }
        var t = prev; prev = cur; cur = t;
    }
    var best = NEG;
    for (j = 0; j < n; j++) {
        if (prev[j] > best)
            best = prev[j];
    }
    if (best <= NEG)
        return 0;
    // Per matched char at most 10+14+18 = 42; scale into 100..499.
    var norm = Math.max(0, best) / (42 * m);
    return Math.round(100 + 399 * Math.min(1, norm));
}

function scoreWord(q, text) {
    if (q === "" || text === "")
        return 0;
    if (text === q)
        return 1000;
    var idx = text.indexOf(q);
    if (idx === 0)
        return 900 - Math.min(50, text.length - q.length);
    if (idx > 0) {
        // Prefer an occurrence at a word start, wherever it is.
        var k = idx;
        while (k >= 0) {
            if (isStart(text, k))
                return 800 - Math.min(100, k);
            k = text.indexOf(q, k + 1);
        }
    }
    if (q.length >= 2) {
        var ini = initialsOf(text);
        if (ini.indexOf(q) === 0)
            return 750 - Math.min(50, ini.length - q.length);
        if (ini.indexOf(q) > 0)
            return 700;
    }
    if (idx > 0)
        return 600 - Math.min(100, idx);
    return subsequence(q, text);
}

function score(q, text) {
    if (!q || !text)
        return 0;
    if (q.indexOf(" ") < 0)
        return scoreWord(q, text);
    var whole = scoreWord(q, text);
    if (whole >= 500)
        return whole;
    var words = q.split(" ").filter(function (w) { return w !== ""; });
    var sum = 0;
    for (var i = 0; i < words.length; i++) {
        var s = scoreWord(words[i], text);
        if (s === 0)
            return whole;
        sum += s;
    }
    return Math.max(whole, Math.round(sum / words.length * 0.9));
}

// Substring tiers only (exact, prefix, word start, inside a word): for long fields such as keyword lists and
// descriptions, where initials and scattered letters match nearly anything.
function substring(q, text) {
    if (!q || !text)
        return 0;
    var idx = text.indexOf(q);
    if (idx < 0)
        return 0;
    if (text === q)
        return 1000;
    if (idx === 0)
        return 900 - Math.min(50, text.length - q.length);
    for (var k = idx; k >= 0; k = text.indexOf(q, k + 1)) {
        if (isStart(text, k))
            return 800 - Math.min(100, k);
    }
    return 600 - Math.min(100, idx);
}
