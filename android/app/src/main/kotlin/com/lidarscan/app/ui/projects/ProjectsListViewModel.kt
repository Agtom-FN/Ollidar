package com.lidarscan.app.ui.projects

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.core.capture.ScanSummary
import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class ProjectsUiState(
    val loading: Boolean = true,
    val projects: List<Project> = emptyList(),
    /**
     * ROUND 27 item 134(b) — project id → why its last "Process again" failed.
     *
     * Kept until the operator taps the card's chip away or starts another run,
     * for the same reason round 23's start-refusal note is kept for four
     * seconds: the answer to something they did has to still be there when they
     * look up.
     */
    val processFailures: Map<String, String> = emptyMap(),
    /**
     * ROUND 22 item 96 — project id → progress (0..1) of a reprocess started
     * from that project's row, so the row can carry a small progress line
     * instead of the operator having to go and find a Jobs screen that Simple
     * mode no longer shows.
     */
    val running: Map<String, Float> = emptyMap(),
)

/**
 * ROUND 28 item 162 — **the empty-scan filter is gone.**
 *
 * ROUND 9 item 33 hid 0-point strays from this list and footnoted them in the
 * hero's aggregate line. The design review (finding P1j) is that this was the
 * wrong trade twice over: **7 of the owner's 74 scans sealed with `points=0`**,
 * and the footnote lived in a subtitle rendered at `maxLines = 1`, so what he
 * actually saw was `2 empt…`. An empty scan is a RESULT — the walk happened and
 * produced nothing — and the operator needs to see it and delete it, not
 * discover it in a file manager. So the list shows every project, empties
 * included, styled as their own thing (ink-mute title, `Empty — no points`,
 * a `bad` EMPTY mark) per §D.5.
 *
 * That takes `keepEmptyScans` and `hiddenEmptyCount` with it. The Settings
 * switch of the same name is untouched and still means what it always meant on
 * the side that matters: `CaptureViewModel` prunes a 0-point project **from
 * disk** at seal unless it is on. Hiding a directory that exists was always a
 * different, weaker promise than not creating it.
 */
class ProjectsListViewModel(
    private val projectStore: ProjectStore,
    /**
     * ROUND 22 item 96 — the row's "Process again". The SAME handle-less
     * `ProcessingRepository.reprocessD6` the seal's auto-process uses, so this
     * is a new caller and not a new pipeline: it takes a directory, opens its
     * own PageStore, holds the per-container lock ROUND 18 added, and is
     * therefore safe to run while a capture is arming.
     *
     * Defaults to a no-op so the project PICKER (which shares this ViewModel)
     * and every JVM test keep working untouched.
     *
     * ROUND 27 item 134(b) — returns the operator-facing FAILURE REASON, or
     * null when the run succeeded.
     *
     * It used to return `Unit`, which is how a failed "Process again" produced
     * literally nothing: the chip appeared, the chip vanished, and the operator
     * was returned to the same "No cloud in memory" screen with no evidence
     * that anything had run at all. The reason exists — `reprocessD6` returns
     * null or `ran = false`, and the engine's `lastError()` says why — and
     * nothing carried it out of this lambda.
     */
    private val reprocess: suspend (java.io.File, (Float) -> Boolean) -> String? = { _, _ -> null },
    /**
     * ROUND 22 items 90 + 96: the scope a reprocess runs in. It must outlive
     * this screen — the operator taps "Process again" and then, quite
     * reasonably, walks away from the tab. `AppContainer.containerScope` in the
     * app; `viewModelScope` when null, which is what tests want.
     */
    private val jobScope: kotlinx.coroutines.CoroutineScope? = null,
) : ViewModel() {

    private val _uiState = MutableStateFlow(ProjectsUiState())
    val uiState: StateFlow<ProjectsUiState> = _uiState.asStateFlow()

    init {
        refresh()
    }

    fun refresh() {
        viewModelScope.launch(Dispatchers.IO) {
            val all = projectStore.list()
            _uiState.update { it.copy(loading = false, projects = all) }
        }
    }

    /**
     * ROUND 22 item 96 — "Process again" from the row's ⋯ menu.
     *
     * Refuses a second run for the same project rather than queueing one: the
     * repository already serialises per container (ROUND 18's lock), so a
     * second tap would simply block a coroutine for the length of the first
     * run and then redo an idempotent job. Saying "it is already running" with
     * a progress line is the same behaviour, visible.
     */
    fun reprocessProject(projectId: String) {
        if (_uiState.value.running.containsKey(projectId)) return
        val project = _uiState.value.projects.firstOrNull { it.id == projectId } ?: return
        _uiState.update {
            it.copy(
                running = it.running + (projectId to 0f),
                // A new attempt clears the last one's verdict. A stale failure
                // line beside a running progress chip is two contradictory
                // statements about the same job.
                processFailures = it.processFailures - projectId,
            )
        }
        (jobScope ?: viewModelScope).launch(Dispatchers.IO) {
            try {
                val why = reprocess(project.directory) { f ->
                    _uiState.update { s -> s.copy(running = s.running + (projectId to f.coerceIn(0f, 1f))) }
                    true
                }
                // ROUND 27 item 134(b): the reason, kept, so the row can say it.
                if (why != null) {
                    _uiState.update { it.copy(processFailures = it.processFailures + (projectId to why)) }
                }
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                // ROUND 22 item 90's rule, applied here too: cancellation is
                // never swallowed and never reported as a failure.
                throw cancelled
            } catch (t: Throwable) {
                _uiState.update {
                    it.copy(
                        processFailures = it.processFailures +
                            (projectId to com.lidarscan.core.Wording.processFailed(
                                t.message?.ifBlank { null } ?: "the scan could not be read",
                            )),
                    )
                }
            } finally {
                _uiState.update { it.copy(running = it.running - projectId) }
                ProjectPreviewCache.invalidate(projectId)
                refresh()
            }
        }
    }

    /** ROUND 27 item 134(b): the operator has read the failure line. */
    fun dismissProcessFailure(projectId: String) {
        _uiState.update { it.copy(processFailures = it.processFailures - projectId) }
    }

    fun delete(projectId: String) {
        viewModelScope.launch(Dispatchers.IO) {
            ProjectPreviewCache.invalidate(projectId)
            projectStore.delete(projectId)
            refresh()
        }
    }
}

/**
 * ROUND 28 item 162, finding P1b — **the row's quality mark, and the honest
 * limit of it.**
 *
 * The review's demand is right and it is the best idea in §D.5: the grade is
 * the ONE field that differs between the owner's 66 scans, and it is the only
 * one that answers "which of these is worth exporting". The problem is that
 * **the grade is not persisted anywhere a list row can reach it.**
 *
 * `ScanSummary.grade` is computed at seal from `pointsCaptured`,
 * `elapsedMillis`, `pathLengthMeters`, `sections`, `trackingDrops`,
 * `posesRecorded`, `worldPointsResolved` and `engineStarted`. Of those, the
 * only ones that survive into `manifest.json` are the point count and the
 * section breaks. The verdict itself goes to a `StateFlow` the summary card
 * reads once and to two log lines (`CaptureViewModel.kt:4700` and `:4893`,
 * `grade=…`) — and a log is not a data source for a screen. There is no
 * `grade` field on [ProjectManifest], no `summary.json`, and
 * `processed/stitch.json` carries the yield audit, not a verdict.
 *
 * So this does not reconstruct the grade. **It states only what the manifest
 * can prove**, and says nothing at all when the manifest proves nothing:
 *
 *  * `EMPTY`  — `pointCountEstimate` is null or zero. Certain, and it is the
 *    case P1j is about.
 *  * `POOR`   — more than [ScanSummary.MAX_SECTIONS_FAIR] sections. Section
 *    breaks ARE persisted (`ProjectManifest.sectionBreaks`), and this is the
 *    same threshold and the same arithmetic the grader uses: everything on
 *    each side of a break sits in a different world frame.
 *  * `FAIR`   — more than [ScanSummary.MAX_SECTIONS_GOOD] sections, or a mount
 *    trim that never converged ([com.lidarscan.core.calib.MountTrim.accuracyIsPoor],
 *    ROUND 12's rule, verbatim).
 *  * `null`   — **no mark.** The manifest records no defect, and "no recorded
 *    defect" is not the same statement as `GOOD`. Density and tracking drops
 *    are what separate GOOD from FAIR and neither is on disk, so printing
 *    either word here would be the app inventing a verdict it did not reach.
 *
 * The `null` arm is also the chip law (§C.4) applied to the grade, which is
 * why it looks right rather than merely honest: the rows with something wrong
 * with them light up, and the quiet ones stay quiet.
 *
 * **To make this the real grade, one nullable `String` field on
 * [ProjectManifest] written from `summary.headline` at seal is the whole job.**
 * Both files belong to other owners this round; see the round-28 report.
 */
object ProjectRowGrade {
    const val EMPTY = "EMPTY"
    const val POOR = "POOR"
    const val FAIR = "FAIR"

    /** Sections = breaks + 1, exactly as [ScanSummary.breaks] defines it in reverse. */
    fun sections(manifest: ProjectManifest): Int = manifest.sectionBreaks.size + 1

    /**
     * The mark for [manifest], or null when the manifest cannot honestly name
     * one.
     *
     * ROUND 28 item 162, second cut — **the sealed verdict wins.**
     *
     * `ProjectManifest.grade` now carries what `ScanSummary.grade` decided at
     * seal, which is the real answer: it saw density, tracking drops, poses
     * recorded, world points resolved and whether the engine ever started, and
     * none of those are on the manifest. Where it is present it is used
     * verbatim, including `GOOD` — which the derivation below can never print,
     * because the two facts that separate GOOD from FAIR are exactly the two it
     * cannot see.
     *
     * The derivation stays for the scans sealed **before** the field existed —
     * which is every scan already on the owner's phone — and it stays
     * deliberately incomplete. It marks what the manifest PROVES (an empty
     * scan, too many sections, a trim that never converged) and returns null
     * otherwise, so an old row is either flagged for a reason on disk or
     * carries no mark at all. Inventing `GOOD` for a scan nothing measured
     * would be the chip-law defect in a new costume: a mark that is always
     * there is a mark that says nothing.
     */
    fun of(manifest: ProjectManifest): String? {
        manifest.grade?.takeIf { it.isNotBlank() }?.let { return it }
        return when {
            manifest.isEmptyScan -> EMPTY
            sections(manifest) > ScanSummary.MAX_SECTIONS_FAIR -> POOR
            sections(manifest) > ScanSummary.MAX_SECTIONS_GOOD -> FAIR
            manifest.mountTrim?.accuracyIsPoor == true -> FAIR
            else -> null
        }
    }
}
