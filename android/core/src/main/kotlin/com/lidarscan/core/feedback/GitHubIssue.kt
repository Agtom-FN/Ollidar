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

    /**
     * The body for **Send diagnostics → GitHub**.
     *
     * It names the zip and says, in as many words, that the app cannot attach
     * it: a URL carries text and nothing else, and pretending otherwise would
     * produce an issue whose "logs attached" line is a lie. [zipName] is the
     * file already sitting in `Downloads/LidarScan` by the time this is built —
     * the sender writes it before it opens anything, which is round 7's rule
     * and the reason this sentence can name a file that certainly exists.
     */
    fun diagnosticsBody(facts: DeviceFacts, zipName: String?, message: String = ""): String =
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
            if (zipName != null) {
                appendLine("**The log bundle is on the phone: `Downloads/LidarScan/$zipName`.**")
            } else {
                appendLine("**The log bundle is on the phone, under `Downloads/LidarScan`.**")
            }
            appendLine()
            appendLine(
                "A link cannot carry a file, so the app could not attach it for you — " +
                    "drag the zip into this issue before you post it.",
            )
        }.trimEnd()

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

    /** The finished URL for **Send diagnostics → GitHub**. */
    fun diagnosticsUrl(facts: DeviceFacts, zipName: String?, message: String = ""): String =
        urlFor(
            if (message.isBlank()) {
                "Diagnostics from Ollidar ${facts.appVersion}"
            } else {
                titleFor(message, facts.appVersion)
            },
            diagnosticsBody(facts, zipName, message),
        )

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
