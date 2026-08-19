package com.lidarscan.core

/**
 * ROUND 22 item 98 — **the wording law, enforceable.**
 *
 * The round-19 "light" guard proved the shape works: an owner correction that
 * lives only in a document is a correction that comes back, and one that lives
 * in a test that fails the build does not. That guard has held since. This is
 * the same mechanism applied to the thing the owner actually complains about
 * on every screen — instructions written by someone who already knows how the
 * app works.
 *
 * ## The law
 *
 *  * an **instruction** is at most [MAX_INSTRUCTION_WORDS] words;
 *  * it may carry **one** detail line of at most [MAX_DETAIL_WORDS] words;
 *  * an **error** says what happened AND what to do;
 *  * no design-document jargon on a screen a normal operator sees.
 *
 * ## What counts as a word
 *
 * Whitespace-separated tokens, minus tokens that are purely punctuation — an
 * em dash between two clauses is a mark, not a word, and counting it would
 * make the law reward writing "and" instead of "—", which is worse English for
 * no gain. Numbers and units count (they are what the reader is here for).
 *
 * ## What is exempt, and why
 *
 * **Log lines are exempt.** They are read by whoever is diagnosing a field
 * failure a year later, and the whole lesson of rounds 17–22 is that they
 * cannot carry too many numbers. `StartHoldTrimGate.refusalLogLine` is the
 * model: a paragraph of numbers, deliberately.
 *
 * **Advanced screens get the lighter pass** ([JARGON] is not checked there,
 * and only the >25-word paragraphs are trimmed). RTK, the calibration wizard
 * and the Mid-360 preflight are for someone who has chosen to be there, and
 * "CRS" is the right word to use with them.
 */
object WordingLaw {

    /** An instruction: six words. "Hold the phone still" is four. */
    const val MAX_INSTRUCTION_WORDS = 6

    /** The one optional detail line under it. */
    const val MAX_DETAIL_WORDS = 12

    /** The advanced-screen ceiling — a paragraph, not an essay. */
    const val MAX_ADVANCED_WORDS = 25

    /**
     * Design-document vocabulary that must never reach a non-advanced screen.
     *
     * Every one of these is real and appears (or appeared) in this codebase's
     * UI strings. They are precise, they are correct, and they mean nothing to
     * a person holding a phone in a flat.
     */
    val JARGON: List<String> = listOf(
        "§", "A12", "A15", "RANSAC", "CRS", "ECEF",
    )

    /** Whitespace-separated tokens that are not purely punctuation. */
    fun words(text: String): List<String> =
        text.trim()
            .split(Regex("\\s+"))
            .filter { token -> token.any { it.isLetterOrDigit() } }

    fun wordCount(text: String): Int = words(text).size

    fun isInstruction(text: String): Boolean = wordCount(text) <= MAX_INSTRUCTION_WORDS

    fun isDetail(text: String): Boolean = wordCount(text) <= MAX_DETAIL_WORDS

    /** The jargon terms present in [text], if any. Case-sensitive: "crs" in "acrs" is not CRS. */
    fun jargonIn(text: String): List<String> = JARGON.filter { text.contains(it) }

    /**
     * An error must say **what happened** and **what to do**. The second half
     * is checked structurally rather than semantically: an actionable sentence
     * contains an imperative the operator can follow, and this is the set the
     * app actually uses. A crude test that catches "Export failed." is worth
     * more than an elegant one that catches nothing.
     */
    val ACTION_WORDS: List<String> = listOf(
        "tap", "press", "open", "hold", "walk", "move", "check", "plug",
        "connect", "try", "wait", "turn", "set", "choose", "pick", "keep",
        "scan", "start", "stop", "retry", "free", "delete", "close", "stand",
    )

    fun isActionable(text: String): Boolean {
        val lower = text.lowercase()
        return ACTION_WORDS.any { verb -> Regex("\\b$verb\\b").containsMatchIn(lower) }
    }
}
