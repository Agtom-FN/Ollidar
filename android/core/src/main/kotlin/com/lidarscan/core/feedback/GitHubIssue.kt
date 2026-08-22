package com.lidarscan.core.feedback

/**
 * ROUND 29 item 173 — **the repo is public, so feedback goes where the code
 * is.**
 *
 * Round 24 item 109 shipped a feedback path with nowhere to send anything: a
 * `multipart/form-data` POST to a server that does not exist, falling back to
 * the share sheet. The owner has now made the repository public
 * (`github.com/Agtom-FN/Ollidar`), which turns "nowhere" into an issue tracker
 * that already has a bug template, already has a queue, and already has the one
 * property a support inbox needs: the person who reads it can reply in the same
 * place.
 *
 * ## Why a URL and not an API call
 *
 * Posting an issue through the REST API needs a token. A token in the app is a
 * token in the APK — every install would carry a credential that can open
 * issues on the owner's repository, which is a spam vector with the owner's
 * name on it, and rotating it means shipping a build. A prefilled `issues/new`
 * URL needs **no** credential: the browser is already signed in as whoever the
 * operator is, the issue is posted under *their* account, and the app never
 * holds anything. The cost is one tap — the operator presses **Submit new
 * issue** himself — and that tap is a feature: nothing leaves the phone until a
 * person has read what is about to leave it.
 *
 * ## Why not `?template=`
 *
 * `.github/ISSUE_TEMPLATE/bug_report.md` exists and the parameter is real, but
 * `template` and `body` are alternatives rather than a pair: GitHub prefills
 * from the template *or* from `body`, and passing both means the template is
 * discarded. Our body is the whole value of this feature — the operator's own
 * words plus the device table he would otherwise have to type out of the
 * Profile page — so the body wins and the template is not requested. The
 * template's questions survive as the shape of [feedbackBody]'s heading.
 *
 * ## The length ceiling
 *
 * GitHub truncates a prefilled issue somewhere near 8 000 characters of URL and
 * gives no warning: the body simply arrives cut, usually mid-table, and the
 * device facts are at the bottom. [urlFor] clamps the body itself so the cut
 * happens where this file chooses it, keeps the device table (which is short
 * and is the part a maintainer cannot reconstruct), and says in the body that
 * it was cut. Everything in `:core`, JVM-testable, because a URL that is wrong
 * is a support conversation that never starts.
 */
object GitHubIssue {

    /** The owner's public repository. */
    const val REPO_URL = "https://github.com/Agtom-FN/Ollidar"

    /** The prefill endpoint. */
    const val NEW_ISSUE_PATH = "/issues/new"

    /**
     * The ceiling, in characters of finished URL.
     *
     * 8 000 rather than 8 192: browsers, the Android intent resolver and
     * GitHub's own front end each take a slice, and a limit that is exactly the
     * documented maximum is a limit that is exceeded by the first proxy.
     */
    const val MAX_URL_CHARS = 8_000

    /** What a clamped body ends with, so a cut is never silent. */
    const val TRUNCATION_NOTE = "…cut here — attach the zip for the rest."

    /** The default title, when the operator's message has no first line to use. */
    fun defaultTitle(appVersion: String): String = "Feedback from Ollidar $appVersion"

    /**
     * The issue title: the operator's **first line**, or [defaultTitle].
     *
     * A title is one line by definition, so taking the first line of what he
     * typed is the closest thing to asking him for one without adding a second
     * field to a sheet that item 165 spent a round reducing to one. Clamped to
     * [MAX_TITLE_CHARS] because GitHub silently truncates longer ones and a
     * title cut mid-word reads as a broken app rather than as a long sentence.
     */
    fun titleFor(message: String, appVersion: String): String {
        val firstLine = message.lineSequence()
            .map { it.trim() }
            .firstOrNull { it.isNotEmpty() }
            ?: return defaultTitle(appVersion)
        return if (firstLine.length <= MAX_TITLE_CHARS) {
            firstLine
        } else {
            firstLine.take(MAX_TITLE_CHARS - 1).trimEnd() + "…"
        }
    }

    /** GitHub's own soft limit on an issue title. */
    const val MAX_TITLE_CHARS = 120

    /**
     * The body for **Send feedback**: what he wrote, the device table, and one
     * line telling him logs can be attached.
     *
     * The device table is the same six facts [DeviceFacts.asText] puts in the
     * bundle — one source, so an issue and a zip can never describe two
     * different phones — rendered as markdown because an issue is read in a
     * browser and `key=value` lines collapse into one paragraph there.
     */
    fun feedbackBody(message: String, facts: DeviceFacts): String = buildString {
        val text = message.trim()
        if (text.isNotEmpty()) {
            appendLine(text)
            appendLine()
        }
        appendLine("---")
        appendLine()
        appendLine(facts.asMarkdownTable())
        appendLine()
        appendLine(ATTACH_HINT)
    }.trimEnd()

    // ── ROUND 35 item 186: the log itself, in the issue ───────────────────
    //
    // > "The send diagnostics to github can it all in the text without zip?"
    //
    // Round 29's GitHub route wrote a zip into Downloads, opened an issue, and
    // asked the operator to go and find the zip and drag it in — a two-device
    // manoeuvre on a phone, and the reason diagnostics issues arrive with no
    // diagnostics in them. A URL cannot carry a file. It can carry several
    // kilobytes of text, and a log IS text.

    /**
     * The tags that get first claim on the budget.
     *
     * A crash, a connection sweep and a *NO DATA* stall are the three things a
     * maintainer opens a diagnostics issue to read, and they are exactly the
     * three a chatty session pushes off the end of a tail: one capture writes
     * hundreds of `[ar]` and `[pushbroom]` lines a minute, and a stall that
     * happened ninety seconds ago is already gone.
     */
    val LOG_PRIORITY_TAGS: List<String> = listOf("[crash]", "[net-debug]", "[session]")

    /** How far back the priority pass looks. */
    const val LOG_PRIORITY_LINES = 40

    /**
     * What one issue's worth of log looks like once it has been chosen.
     *
     * [lines] is ready to print — it already carries the gap markers, so the
     * renderer has no decisions left to make and the *test* can read exactly
     * what the operator will.
     */
    data class LogExcerpt(
        val lines: List<String>,
        /** How many real log lines are in [lines] (markers do not count). */
        val kept: Int,
        /** How many there were altogether. */
        val total: Int,
    ) {
        val isEmpty: Boolean get() = kept == 0

        /** True when [lines] is a single unbroken run off the end of the log. */
        val isContiguous: Boolean get() = lines.none { it.startsWith(GAP_PREFIX) }
    }

    /** How an omitted stretch announces itself inside the code block. */
    const val GAP_PREFIX = "… "

    /**
     * Choose at most [budgetChars] characters of [log], **whole lines only**.
     *
     * Two passes, which is item 186(b2):
     *
     *  1. **Priority.** Walk the last [LOG_PRIORITY_LINES] lines that carry one
     *     of [LOG_PRIORITY_TAGS], newest first, taking them while they fit in
     *     *half* the budget. Half rather than all of it: an issue that is
     *     nothing but forty `[net-debug]` lines has thrown away the context
     *     that says what the operator was doing.
     *  2. **Tail.** Fill whatever is left with the most recent lines, newest
     *     first, skipping any the first pass already took.
     *
     * The result is emitted in the log's own order, with a marker wherever a
     * stretch was skipped, so a reader can never mistake two distant lines for
     * consecutive ones. Nothing is ever cut mid-line: a truncated log line is a
     * line with a plausible wrong value in it, which is worse than an absent
     * one.
     */
    fun logExcerpt(log: String, budgetChars: Int): LogExcerpt {
        val all = log.lineSequence().filter { it.isNotBlank() }.toList()
        if (all.isEmpty() || budgetChars <= 0) return LogExcerpt(emptyList(), 0, all.size)

        val chosen = sortedSetOf<Int>()
        var spent = 0
        fun take(index: Int, ceiling: Int): Boolean {
            if (index in chosen) return true
            val cost = all[index].length + 1
            if (spent + cost > ceiling) return false
            chosen += index
            spent += cost
            return true
        }

        // ── pass 1: the lines somebody opened this issue to read ───────────
        val priorityCeiling = budgetChars / 2
        all.indices
            .filter { i -> LOG_PRIORITY_TAGS.any { all[i].contains(it) } }
            .takeLast(LOG_PRIORITY_LINES)
            .asReversed()
            .forEach { take(it, priorityCeiling) }

        // ── pass 2: plain tail, into whatever is left ──────────────────────
        for (i in all.indices.reversed()) {
            // `take` returns true for a line pass 1 already paid for, so the
            // tail walks straight through those and only stops when a line
            // genuinely does not fit.
            if (!take(i, budgetChars)) break
        }

        if (chosen.isEmpty()) return LogExcerpt(emptyList(), 0, all.size)

        val out = mutableListOf<String>()
        var previous = -1
        for (i in chosen) {
            if (previous >= 0 && i != previous + 1) {
                out += "$GAP_PREFIX${i - previous - 1} lines omitted …"
            }
            out += all[i]
            previous = i
        }
        return LogExcerpt(out, chosen.size, all.size)
    }

    /** The honest sentence at the top of the code block. */
    fun logExcerptHeading(excerpt: LogExcerpt): String = when {
        excerpt.isContiguous ->
            "Last ${excerpt.kept} lines of the log " +
                "(of ${excerpt.total}) — full log available via Save to phone."
        else ->
            "${excerpt.kept} of ${excerpt.total} log lines — the most recent, plus earlier " +
                "${LOG_PRIORITY_TAGS.joinToString("/")} lines. Gaps are marked. " +
                "Full log available via Save to phone."
    }

    /**
     * The body for **Send diagnostics → GitHub**.
     *
     * ROUND 35 item 186(a): the device table, and then the log itself in a
     * fenced block. Round 29's version named a zip in `Downloads` and admitted
     * the app could not attach it — a true sentence about a design the owner
     * has now removed.
     */
    fun diagnosticsBody(facts: DeviceFacts, excerpt: LogExcerpt?, message: String = ""): String =
        buildString {
            val text = message.trim()
            if (text.isNotEmpty()) {
                appendLine(text)
                appendLine()
            }
            appendLine("Diagnostics from Ollidar ${facts.appVersion}.")
            appendLine()
            appendLine(facts.asMarkdownTable())
            appendLine()
            if (excerpt == null || excerpt.isEmpty) {
                appendLine(NO_LOG_NOTE)
            } else {
                appendLine(logExcerptHeading(excerpt))
                appendLine()
                appendLine(LOG_FENCE_OPEN)
                excerpt.lines.forEach { appendLine(it) }
                appendLine(LOG_FENCE_CLOSE)
            }
        }.trimEnd()

    /** The opening fence. `text` rather than a language: a log is not a language. */
    const val LOG_FENCE_OPEN = "```text"

    /**
     * …and the closing one, which is **not** the same string.
     *
     * CommonMark forbids an info string on a closing fence: ```` ```text ````
     * at the bottom of a block does not close it, it opens another one. The
     * first cut used one constant for both and the issue rendered with the log
     * inside a block that never ended — caught by opening the URL the app
     * actually launched, which is the only place that particular bug is
     * visible.
     */
    const val LOG_FENCE_CLOSE = "```"

    /** When there is no log at all — a first launch, or a cleared one. */
    const val NO_LOG_NOTE = "The capture log is empty on this device."

    /**
     * The finished URL for **Send diagnostics → GitHub**, log and all.
     *
     * The budget cannot be computed in one pass: the clamp is on the
     * **encoded** URL and a log line encodes at roughly two and a half times
     * its length (every space, colon, bracket and equals sign is three
     * characters once escaped), and by exactly how much depends on the line.
     * So the head — the part that always ships — is measured first, and the
     * excerpt is then grown into what is left and shrunk by a quarter until it
     * fits. Half a dozen iterations, on a string of a few kilobytes, once per
     * tap.
     */
    fun diagnosticsUrl(facts: DeviceFacts, log: String, message: String = ""): String {
        val title = if (message.isBlank()) {
            "Diagnostics from Ollidar ${facts.appVersion}"
        } else {
            titleFor(message, facts.appVersion)
        }
        val base = "$REPO_URL$NEW_ISSUE_PATH?title=${encode(title)}&body="
        val room = MAX_URL_CHARS - base.length
        val head = diagnosticsBody(facts, null, message)
        if (room <= 0) return base
        if (encode(head).length >= room) return urlFor(title, head)

        // Bisection on the decoded budget rather than a shrink-by-a-quarter
        // walk: the walk fitted, but it stopped at the first budget that
        // happened to fit and left a kilobyte of the ceiling unused — which is
        // fifteen more log lines that a maintainer does not get. Fourteen
        // halvings put it within a character of the largest excerpt that fits,
        // for fourteen encodings of a few kilobytes, once per tap.
        var low = 0
        var high = room - encode(head).length
        var best: String? = null
        repeat(14) {
            val mid = (low + high) / 2
            if (mid > low) {
                val body = diagnosticsBody(facts, logExcerpt(log, mid), message)
                val encoded = encode(body)
                if (encoded.length <= room) {
                    best = encoded
                    low = mid
                } else {
                    high = mid
                }
            }
        }
        return base + (best ?: encode(head))
    }

    /** The one line under a feedback body. */
    const val ATTACH_HINT =
        "Logs can be attached: Settings → Profile → Send diagnostics → Save to phone, " +
            "then drag the zip in."

    /**
     * `https://github.com/Agtom-FN/Ollidar/issues/new?title=…&body=…`, clamped
     * to [MAX_URL_CHARS].
     *
     * The clamp runs on the **decoded** body and then re-encodes, because
     * cutting an encoded string can land inside a `%E2%80%A6` escape and
     * produce a URL that is not merely short but invalid. Binary search would
     * be neater; a body is at most a few kilobytes and the loop below runs a
     * handful of times, so the readable version wins.
     */
    fun urlFor(title: String, body: String): String {
        val base = "$REPO_URL$NEW_ISSUE_PATH?title=${encode(title)}&body="
        val room = MAX_URL_CHARS - base.length
        if (room <= 0) return base
        if (encode(body).length <= room) return base + encode(body)

        // Percent-encoding expands by at most 12 characters per character
        // (a 4-byte code point is 4 × "%XX"), so a first guess of room/3 is
        // always safe and usually close.
        var keep = (room / 3).coerceAtMost(body.length)
        while (keep > 0) {
            val candidate = body.take(keep).trimEnd() + "\n\n" + TRUNCATION_NOTE
            val encoded = encode(candidate)
            if (encoded.length <= room) return base + encoded
            keep -= (keep / 8).coerceAtLeast(1)
        }
        return base + encode(TRUNCATION_NOTE)
    }

    /** The finished URL for **Send feedback**. */
    fun feedbackUrl(message: String, facts: DeviceFacts): String =
        urlFor(titleFor(message, facts.appVersion), feedbackBody(message, facts))

    /**
     * RFC 3986 percent-encoding of a query **value**.
     *
     * Hand-rolled rather than `java.net.URLEncoder` for two reasons that are
     * both bugs somebody has shipped: `URLEncoder` is
     * `application/x-www-form-urlencoded`, so it writes a space as `+` — which
     * GitHub renders literally as a plus inside a code fence — and it leaves
     * `*` unescaped, which is a markdown emphasis marker. Everything outside
     * the unreserved set goes out as UTF-8 bytes in `%XX`, uppercase, which is
     * what every server normalises to anyway.
     *
     * `:core` is plain Kotlin/JVM (see `core/build.gradle.kts`), so
     * `android.net.Uri` is not reachable from here — and that is the point:
     * this is the half of the feature a test can hold still.
     */
    fun encode(value: String): String {
        val out = StringBuilder(value.length + 16)
        for (byte in value.toByteArray(Charsets.UTF_8)) {
            val c = byte.toInt().toChar()
            if (c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c in UNRESERVED_PUNCTUATION) {
                out.append(c)
            } else {
                out.append('%').append(HEX[(byte.toInt() shr 4) and 0xF]).append(HEX[byte.toInt() and 0xF])
            }
        }
        return out.toString()
    }

    private const val UNRESERVED_PUNCTUATION = "-_.~"
    private const val HEX = "0123456789ABCDEF"
}
