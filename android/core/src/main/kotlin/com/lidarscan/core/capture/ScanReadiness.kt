package com.lidarscan.core.capture

/**
 * ROUND 28 item 158 — **the entire pre-flight, in three lines, with no chips.**
 *
 * The idle Scan screen used to state readiness six ways at once: a floating
 * status card carrying `D6` + `00:00` + `0 pts · 0.0 m` + a filled mint
 * `3D TRACKING` chip + an outlined `Idle` chip, and below the viewport a mount
 * pill, a Re-zero pill, a scan-name pill with a status word inside it, a `Diag`
 * pill and a `New capture` pill — five distinct visual treatments in two ragged
 * rows, none of which was the primary action, bleeding off the right edge.
 *
 * `3D TRACKING` and `Idle` were the same fact in two shapes. `00:00`, `0 pts`
 * and `0.0 m` were three readouts whose entire content was "nothing has
 * happened", occupying the most valuable position on the screen.
 *
 * §D.1 replaces all of it with three rows, and the rule that makes three rows
 * enough is this: **each row states its own state and carries its own fix.**
 *
 * | state | the row renders |
 * |---|---|
 * | [State.GOOD] | `● Sensor · COIN-D6 connected` |
 * | [State.WARN] | `● Mount · Not set` + `Re-zero before scanning.` + a Secondary `Re-zero` |
 * | [State.BAD] | `● Sensor · Not found` + `Plug it in, then retry.` + a Secondary `Retry` |
 *
 * And the FAB reads the same three rows rather than a separate predicate, which
 * is what stops the button and the page ever disagreeing — the failure mode the
 * old screen had constantly, where a disabled FAB sat above a card that said
 * everything was fine.
 *
 * The model lives in `:core` because "when may this app start recording" is the
 * single most consequential question it asks and it must be answerable without
 * an emulator.
 */
object ScanReadiness {

    /**
     * The three colours a row may be, and nothing else.
     *
     * There is deliberately no `UNKNOWN`. A readiness row whose state is not
     * known yet is not ready, and rendering a fourth neutral state would put
     * the screen back where it started — saying something without committing to
     * it. A check still running is [WARN] with a detail line that says so.
     */
    enum class State { GOOD, WARN, BAD }

    /**
     * @param title one word the operator would say: `Sensor`, `Mount`, `Tracking`.
     * @param value the right-aligned Meta readout, ≤4 words.
     * @param detail the ≤6-word fix, present only when [state] is not [State.GOOD].
     * @param actionLabel the row's own Secondary button, or null.
     */
    data class Row(
        val title: String,
        val state: State,
        val value: String,
        val detail: String? = null,
        val actionLabel: String? = null,
    ) {
        init {
            require(title.isNotBlank()) { "a readiness row must name what it is about" }
        }
    }

    /**
     * **The FAB is enabled when nothing is [State.BAD].**
     *
     * Not "when everything is GOOD": a warn row is a scan that will work and be
     * worse, and round 12's rule — an app that will not start is worse than a
     * warned one — applies to the button exactly as it applies to the gate. An
     * unset mount means the pushbroom runs on the CAD nominal, which is a real
     * scan with a known error, not a refusal.
     *
     * A BAD row is different in kind: there is no sensor on the cable, so there
     * is nothing to record.
     */
    fun canStart(rows: List<Row>): Boolean = rows.none { it.state == State.BAD }

    /**
     * The one row that may be drawn in `bad` colour, or null.
     *
     * §D.1: *"when any is bad, the FAB is disabled and the offending row is the
     * only thing in bad colour on screen."* Returning the row rather than a
     * boolean is what lets the screen honour the second half — with two things
     * failing, the first is the one that gets the colour, because a screen with
     * two red rows is a screen that has stopped ranking its own problems.
     */
    fun blocker(rows: List<Row>): Row? = rows.firstOrNull { it.state == State.BAD }

    /**
     * The status-bar line: `COIN-D6 · Ready`, or the blocker's own words.
     *
     * One clause, because it sits beside an icon button in a 56 dp bar and
     * finding P1d is what happens to a subtitle that carries four.
     */
    fun statusLine(sensorName: String?, rows: List<Row>): String {
        val blocked = blocker(rows)
        if (blocked != null) return blocked.value
        val name = sensorName?.takeIf { it.isNotBlank() } ?: return "Ready"
        return "$name · Ready"
    }
}
