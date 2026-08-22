package com.lidarscan.core.feedback

/**
 * ROUND 24 item 109 — **sending a log to somebody, when there is nobody to
 * send it to yet.**
 *
 * The owner wants SEND LOGS and FEEDBACK on the new Profile page. There is no
 * confirmed server endpoint. Those two facts are not in conflict as long as the
 * *decision* about where a bundle goes is a value rather than a branch buried
 * in a ViewModel: this file is that value, and the whole of it is testable on a
 * bare JVM.
 *
 * ## The two paths, and why both are real
 *
 *  * **Configured** — a cloud server URL and token exist (the same two fields
 *    D3 has had since the cloud processing mode; there is no second pair of
 *    credentials to set up and no second thing to get wrong). The bundle is
 *    POSTed as `multipart/form-data` to [FeedbackEndpoint.PATH] under that
 *    base URL.
 *  * **Not configured** — the Android share sheet, with the zip attached. Mail
 *    it, put it in Drive, drop it in a chat. This is the path that will
 *    actually be used on the owner's phone tomorrow, and it is the reason the
 *    feature is worth shipping before the endpoint exists.
 *
 * The zip is written to `Downloads/LidarScan` **first**, in both paths, and it
 * stays there whatever happens next. That is ROUND 7's rule ("no user-triggered
 * file operation ends in neither a visible success with a path nor a visible
 * failure") applied to a hand-off that this time genuinely may not complete:
 * the share sheet has no result callback and the server may be unreachable, so
 * the one outcome the app can *guarantee* is a file the operator can find.
 */
object FeedbackEndpoint {

    /**
     * Appended to the configured cloud base URL.
     *
     * Distinct from the processing service's own paths on purpose: a feedback
     * bundle is not a job, it must never enter the job queue, and a deployment
     * that has one and not the other must be able to answer 404 on this without
     * breaking anything else.
     */
    const val PATH = "/feedback"

    /** The multipart field the zip is sent under. */
    const val FILE_FIELD = "bundle"

    /** The multipart field the operator's typed message is sent under. */
    const val MESSAGE_FIELD = "message"

    /** The multipart field the device/app summary is sent under, as plain text. */
    const val INFO_FIELD = "info"

    /**
     * `<base>/feedback`, with exactly one slash between them.
     *
     * A base URL typed by a human ends in a slash about half the time, and
     * `"$base$PATH"` produces `https://host//feedback` for the other half —
     * which some reverse proxies normalise and some 404.
     */
    fun urlFor(baseUrl: String): String = baseUrl.trimEnd('/') + PATH
}

/**
 * Where a bundle is going, decided before anything is packed.
 *
 * Two values rather than a nullable URL because the UI has to say which one is
 * about to happen *before* the operator commits (see [FeedbackWording.noteFor]),
 * and "null means share sheet" is the kind of implicit rule that survives
 * exactly until someone adds a third path.
 */
enum class FeedbackRoute {
    /** POST to the configured server. */
    SERVER,

    /** Hand the zip to the system share sheet. */
    SHARE,

    // ── ROUND 29 item 173: two destinations the operator picks by hand ─────

    /**
     * Write the zip to `Downloads/LidarScan` and open a prefilled GitHub issue.
     *
     * The app does **not** post anything: a URL cannot carry a file and this
     * app holds no GitHub credential (see [GitHubIssue]). What it guarantees is
     * the zip on disk and a browser open on a form whose body already names it.
     */
    GITHUB,

    /** Write the zip to `Downloads/LidarScan` and stop there. */
    SAVE,
    ;

    /** True where the app is only responsible for the file. */
    val isLocal: Boolean get() = this == SAVE || this == GITHUB
}

/**
 * The configured destination, or the absence of one.
 *
 * Both fields must be non-blank for [route] to be [FeedbackRoute.SERVER]: a URL
 * with no token is a request that will 401, and offering "Send" for it would be
 * the app promising something it knows will fail.
 */
data class FeedbackConfig(
    val serverUrl: String = "",
    val token: String = "",
) {
    val route: FeedbackRoute
        get() = if (serverUrl.isNotBlank() && token.isNotBlank()) {
            FeedbackRoute.SERVER
        } else {
            FeedbackRoute.SHARE
        }

    /** The full URL this config posts to, or null when there is no server. */
    fun url(): String? =
        if (route == FeedbackRoute.SERVER) FeedbackEndpoint.urlFor(serverUrl.trim()) else null
}

/**
 * What a send actually did.
 *
 * [downloadsPath] is non-null whenever the zip reached `Downloads/LidarScan`,
 * which is every path except a packaging failure — so the failure message can
 * always name a file, and the success message can too.
 */
data class FeedbackResult(
    val sent: Boolean,
    val route: FeedbackRoute,
    val downloadsPath: String? = null,
    /** The reason, for the log. Never shown raw — see [FeedbackWording.resultFor]. */
    val failure: String? = null,
)

/**
 * ROUND 24 item 109 — the sentences, under the ROUND 22 wording law.
 *
 * Every instruction here is six words or fewer and every detail line is twelve
 * or fewer, checked by `Round24WordingTest` the same way `Wording`'s are. The
 * privacy note is one line and it is the whole truth: the bundle is the app's
 * own capture log plus the device summary this file builds, and nothing else
 * is read from the phone.
 */
object FeedbackWording {

    /** The Profile page's two actions. */
    const val SEND_LOGS = "Send logs"
    const val SEND_FEEDBACK = "Send feedback"

    /** The one line above both of them. */
    const val PRIVACY_NOTE = "Sends your logs and device info."

    /** The text box's placeholder. */
    const val FEEDBACK_PLACEHOLDER = "What went wrong?"

    /** While the job runs. */
    const val SENDING = "Sending…"

    /** The success. Two words, because it needs no more. */
    const val SENT = "Sent."

    /**
     * The failure, in ROUND 7's shape: what happened, and where the thing is
     * now. It never asks the operator to retry into a server that is not there
     * — it tells them the file exists and can be sent by hand.
     */
    const val NOT_SENT = "Could not send. Saved to Downloads."

    /** Which path is about to be taken, said before the tap. */
    fun noteFor(route: FeedbackRoute): String = when (route) {
        FeedbackRoute.SERVER -> "Sends to your server."
        FeedbackRoute.SHARE -> "Opens the share sheet."
        FeedbackRoute.GITHUB -> DIAGNOSTICS_GITHUB_DETAIL
        FeedbackRoute.SAVE -> DIAGNOSTICS_SAVE_DETAIL
    }

    /**
     * ROUND 29 item 173 — the Send-diagnostics row's second line.
     *
     * It used to state the route the app had already chosen, which was honest
     * while there was exactly one. There are three now and the operator picks,
     * so the line says that instead of predicting it.
     */
    const val CHOOSE_NOTE = "Choose where it goes."

    /** The compose sheet's button, once feedback became a GitHub issue. */
    const val OPEN_GITHUB = "Open GitHub"

    /** The fourth diagnostics door, offered only when a server is configured. */
    const val DIAGNOSTICS_SERVER = "Your server"

    /**
     * The last word, from a finished [FeedbackResult].
     *
     * ROUND 29 item 173: a local route never says "Sent." — the app has not
     * sent anything, it has written a file (and possibly opened a browser on a
     * form the operator still has to submit). Claiming otherwise is the exact
     * failure round 7 named, one destination later.
     */
    fun resultFor(result: FeedbackResult): String = when {
        !result.sent -> if (result.route.isLocal) NOT_SAVED else NOT_SENT
        result.route == FeedbackRoute.GITHUB -> OPENED_GITHUB
        result.route == FeedbackRoute.SAVE -> savedTo(result.downloadsPath ?: "Downloads")
        else -> SENT
    }

    /** The chooser's own title on the share path. */
    const val SHARE_TITLE = "Send logs"

    // ── ROUND 29 item 173: the three doors out of Send diagnostics ─────────

    /**
     * The sheet's title, and its three options.
     *
     * Three rather than two because all three are genuinely different
     * destinations and the operator knows which one he wants before he taps:
     * the issue tracker (where the owner reads), the phone (where a file can be
     * found again), and whatever app he already mails logs with. Item 165's
     * rule holds — one job per row, and the row says which.
     */
    const val DIAGNOSTICS_TITLE = "Send diagnostics"
    const val DIAGNOSTICS_GITHUB = "GitHub"
    const val DIAGNOSTICS_GITHUB_DETAIL = "Opens an issue in your browser."
    const val DIAGNOSTICS_SAVE = "Save to phone"
    const val DIAGNOSTICS_SAVE_DETAIL = "Puts the zip in Downloads."
    const val DIAGNOSTICS_SHARE = "Share…"
    const val DIAGNOSTICS_SHARE_DETAIL = "Mail it, or put it in Drive."

    /** **Send feedback**, which no longer packs anything: it opens a browser. */
    const val FEEDBACK_GITHUB_DETAIL = "Opens an issue in your browser."

    /**
     * What the GitHub path says when it has finished.
     *
     * It is deliberately NOT "Sent.": the app has opened a browser, and the
     * issue is posted when the operator presses the button in it. Claiming a
     * send the app did not make is the failure mode round 7 named.
     */
    const val OPENED_GITHUB = "Opened GitHub. Post it there."

    /** The zip landed and nothing else was attempted. */
    fun savedTo(path: String): String = "Saved to $path"

    /** The zip could not be written at all. */
    const val NOT_SAVED = "Could not save the zip."

    val ALL: List<String> = listOf(
        SEND_LOGS,
        SEND_FEEDBACK,
        FEEDBACK_PLACEHOLDER,
        SENDING,
        SENT,
        SHARE_TITLE,
        DIAGNOSTICS_TITLE,
        DIAGNOSTICS_GITHUB,
        DIAGNOSTICS_SAVE,
        DIAGNOSTICS_SHARE,
        OPENED_GITHUB,
        NOT_SAVED,
        OPEN_GITHUB,
        DIAGNOSTICS_SERVER,
        CHOOSE_NOTE,
        noteFor(FeedbackRoute.SERVER),
        noteFor(FeedbackRoute.SHARE),
    )

    /** The detail lines (twelve words). */
    val DETAILS: List<String> = listOf(
        PRIVACY_NOTE,
        NOT_SENT,
        DIAGNOSTICS_GITHUB_DETAIL,
        DIAGNOSTICS_SAVE_DETAIL,
        DIAGNOSTICS_SHARE_DETAIL,
        FEEDBACK_GITHUB_DETAIL,
    )
}

/**
 * ROUND 29 item 173(b/c) — **the chooser, as a value.**
 *
 * The three (or four) doors out of *Send diagnostics* are a list rather than
 * four `if`s in a composable, for the same reason `FeedbackConfig` was a value
 * in round 24: the interesting question — *which doors exist on this phone* —
 * is then answerable by a JVM test instead of by an emulator screenshot.
 *
 * The order is fixed and is not alphabetical. GitHub is first because it is
 * where the owner reads; **Save to phone** is second because it is the one that
 * always works; Share is third because it is the general case; the server is
 * last and conditional, because a phone with no cloud fields filled in must not
 * be offered a destination that will 401.
 */
object DiagnosticsChooser {

    fun optionsFor(serverConfigured: Boolean): List<FeedbackRoute> = buildList {
        add(FeedbackRoute.GITHUB)
        add(FeedbackRoute.SAVE)
        add(FeedbackRoute.SHARE)
        if (serverConfigured) add(FeedbackRoute.SERVER)
    }

    /** The row's title. */
    fun titleFor(route: FeedbackRoute): String = when (route) {
        FeedbackRoute.GITHUB -> FeedbackWording.DIAGNOSTICS_GITHUB
        FeedbackRoute.SAVE -> FeedbackWording.DIAGNOSTICS_SAVE
        FeedbackRoute.SHARE -> FeedbackWording.DIAGNOSTICS_SHARE
        FeedbackRoute.SERVER -> FeedbackWording.DIAGNOSTICS_SERVER
    }

    /** The row's one detail line. */
    fun detailFor(route: FeedbackRoute): String = when (route) {
        FeedbackRoute.SHARE -> FeedbackWording.DIAGNOSTICS_SHARE_DETAIL
        else -> FeedbackWording.noteFor(route)
    }

    /** The test tag each row carries, so one list drives the suite too. */
    fun testTagFor(route: FeedbackRoute): String = "diagnostics${route.name.lowercase()
        .replaceFirstChar { it.uppercase() }}"
}

/**
 * The device/app summary that travels with every bundle — and the ONLY thing
 * this feature reads from the phone that the log does not already contain.
 *
 * Deliberately a plain data class of things the app already knows and already
 * prints somewhere: the version footer's two numbers, the `Build.MODEL` the
 * Settings screen could show anyway, the Android release, the engine ABI the
 * diagnostics sheet reports, and two counts derived from the app's own project
 * directory. No identifiers, no account, no location, no advertising id,
 * nothing that is not visible to the operator on the Profile page above the
 * button they are about to press.
 */
data class DeviceFacts(
    val appVersion: String = "",
    val versionCode: Int = 0,
    val deviceModel: String = "",
    val androidVersion: String = "",
    val engineAbi: Int = 0,
    val scanCount: Int = 0,
    val storageBytes: Long = 0L,
) {
    /**
     * The summary, as the plain text that goes into the zip and the multipart
     * `info` field. One `key=value` per line: it is read by a person and
     * grepped by a person, which is the same posture `CaptureLog` takes.
     */
    fun asText(): String = buildString {
        appendLine("app=$appVersion ($versionCode)")
        appendLine("device=$deviceModel")
        appendLine("android=$androidVersion")
        appendLine("engineAbi=$engineAbi")
        appendLine("scans=$scanCount")
        appendLine("scanStorageBytes=$storageBytes")
    }

    /** `1.2 GB` / `840 MB` / `12 KB` — the Profile card's storage read-out. */
    fun storageLabel(): String = formatBytes(storageBytes)

    /**
     * ROUND 29 item 173 — **the same six facts, for a browser.**
     *
     * [asText] is read in a terminal and grepped; a GitHub issue is read in a
     * browser, where six `key=value` lines collapse into one paragraph and the
     * maintainer has to re-split them by eye. Same fields, same order, same
     * source — a second list of "what we tell support about this phone" is a
     * second list that can disagree with the bundle sitting next to it.
     */
    fun asMarkdownTable(): String = buildString {
        appendLine("| field | value |")
        appendLine("| --- | --- |")
        appendLine("| app | $appVersion ($versionCode) |")
        appendLine("| device | ${deviceModel.ifBlank { "unknown" }} |")
        appendLine("| android | ${androidVersion.ifBlank { "unknown" }} |")
        appendLine("| engine ABI | $engineAbi |")
        appendLine("| scans | $scanCount |")
        append("| scan storage | ${storageLabel()} |")
    }

    companion object {
        fun formatBytes(bytes: Long): String = when {
            bytes >= 1_000_000_000L -> "%.1f GB".format(bytes / 1_000_000_000.0)
            bytes >= 1_000_000L -> "%.0f MB".format(bytes / 1_000_000.0)
            bytes >= 1_000L -> "%.0f KB".format(bytes / 1_000.0)
            else -> "$bytes B"
        }
    }
}

/**
 * `multipart/form-data`, built by hand.
 *
 * `:core` is a plain Kotlin/JVM module (see `core/build.gradle.kts`) and this
 * has to stay that way — it is what lets the whole feedback contract be tested
 * without an emulator. A multipart body is a boundary, a few headers and a
 * trailing `--boundary--`; the parts that are easy to get wrong are exactly the
 * parts a test can pin, and all three of them are:
 *
 *  * **CRLF, everywhere.** RFC 7578 inherits RFC 2046's line endings, and a
 *    body written with `\n` is accepted by some servers and rejected by
 *    others — the worst possible failure mode for a diagnostic upload.
 *  * **The final boundary carries trailing dashes.** Without them the last
 *    part is unterminated and the parser either blocks or drops it.
 *  * **The file part is raw bytes.** Nothing here decodes the zip.
 */
object Multipart {

    /** A boundary that cannot occur in a zip's bytes as a line of its own. */
    fun boundary(seed: Long): String = "----LidarScanFeedback${seed.toString(16)}"

    fun contentType(boundary: String): String = "multipart/form-data; boundary=$boundary"

    private const val CRLF = "\r\n"

    /**
     * The body for one file part plus [fields] text parts.
     *
     * Text parts come first so a server that streams the body sees the message
     * and the device summary before it has to buffer the archive — which for a
     * five-megabyte log is the difference between a useful 413 and a useless
     * one.
     */
    fun body(
        boundary: String,
        fields: Map<String, String>,
        fileField: String,
        fileName: String,
        fileMime: String,
        fileBytes: ByteArray,
    ): ByteArray {
        val head = buildString {
            for ((name, value) in fields) {
                append("--").append(boundary).append(CRLF)
                append("Content-Disposition: form-data; name=\"").append(name).append("\"").append(CRLF)
                append("Content-Type: text/plain; charset=utf-8").append(CRLF)
                append(CRLF)
                append(value).append(CRLF)
            }
            append("--").append(boundary).append(CRLF)
            append("Content-Disposition: form-data; name=\"").append(fileField)
                .append("\"; filename=\"").append(fileName).append("\"").append(CRLF)
            append("Content-Type: ").append(fileMime).append(CRLF)
            append(CRLF)
        }.toByteArray(Charsets.UTF_8)
        val tail = (CRLF + "--" + boundary + "--" + CRLF).toByteArray(Charsets.UTF_8)

        val out = ByteArray(head.size + fileBytes.size + tail.size)
        head.copyInto(out, 0)
        fileBytes.copyInto(out, head.size)
        tail.copyInto(out, head.size + fileBytes.size)
        return out
    }
}
