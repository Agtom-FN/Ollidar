package com.lidarscan.core.render

/**
 * ROUND 27 item 141 — **a default that only reached state nobody had.**
 *
 * The owner, on 0.9.11: *"height rgb showing not working too."* Round 26 made
 * Turbo the height colormap by changing `DisplayParams.height`'s default and
 * `CAPTURE_HEIGHT_COLORMAP`, and wrote in its own resolution that *"a project
 * with a saved colormap deserialises it and never reaches any of these lines"*
 * — which was stated as a reassurance and is in fact the defect. Every project
 * on the owner's phone has a saved colormap. Every one of them says
 * `GRAYSCALE`, because that is what the app wrote before round 26. So the new
 * default reached exactly the projects that did not exist yet, and the owner
 * switched to Height on a real scan and got grey.
 *
 * A default is not a migration. This is the migration.
 *
 * ## Why it is versioned rather than conditional
 *
 * "If height is grayscale, make it Turbo" run on every read would take the
 * choice away from an operator who deliberately picks grayscale — height in
 * grayscale is a legitimate thing to want, it is the flattest reading of a
 * ceiling — and it would take it away again every single time they reopened the
 * project. So the rule runs **once per project**: a stamp records the highest
 * migration already applied, and a project that has been through
 * [HEIGHT_TURBO] is never touched again whatever its colormap says.
 *
 * The stamp is a nullable additive field on the manifest, so the schema version
 * does not move and a manifest written by 0.9.12 still reads on 0.9.11.
 */
object DisplayMigrations {

    /**
     * Migration 1 — a persisted **height** colormap of `GRAYSCALE` becomes
     * `TURBO`.
     *
     * Only height, and only grayscale. Intensity's grayscale is round 10 item
     * 39's deliberate answer (`CAPTURE_COLORMAP`) and is not a leftover;
     * `SPECTRUM` and `THERMAL` on height are choices somebody made on purpose.
     */
    const val HEIGHT_TURBO = 1

    /** The migration level a manifest written today carries. */
    const val CURRENT = HEIGHT_TURBO

    /** The log line, so a field report can show the migration happening. */
    const val LOG_LINE = "[store] height colormap migrated grayscale→turbo"

    /**
     * [params] as they should be READ, given the migration level already
     * stamped on them.
     *
     * @param applied the stamp, or null for a manifest written before the stamp
     *   existed — which is every project the owner already has, and therefore
     *   the arm that matters.
     * @return the migrated params, and whether anything actually changed (so
     *   the caller can log it exactly once rather than on every read).
     */
    fun migrate(params: DisplayParams, applied: Int?): Result {
        if ((applied ?: 0) >= HEIGHT_TURBO) return Result(params, changed = false)
        if (params.height.colormap != Colormap.GRAYSCALE) {
            // Nothing to change, but the project is still now AT this level:
            // a spectrum height that gets stamped will not be re-examined, and
            // an operator who picks grayscale afterwards keeps it.
            return Result(params.copy(migration = CURRENT), changed = false)
        }
        return Result(
            params.copy(
                height = params.height.copy(colormap = Colormap.TURBO),
                migration = CURRENT,
            ),
            changed = true,
        )
    }

    /** [params] stamped as fully migrated, for the write path. */
    fun stamp(params: DisplayParams): DisplayParams =
        if (params.migration == CURRENT) params else params.copy(migration = CURRENT)

    data class Result(val params: DisplayParams, val changed: Boolean)
}
