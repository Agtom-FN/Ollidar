package com.lidarscan.core.store

import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile

/**
 * Creates/lists/opens/deletes `.lscan` project directories (Tech Spec
 * §3.11). One instance owns one root directory; see
 * [FileProjectStore] for the disk-backed implementation B1 ships, and
 * [com.lidarscan.core.store.ProjectStore.Companion] callers such as the
 * `:app` DI-lite container for how the root is chosen.
 */
interface ProjectStore {
    /** All projects under the root, newest first. Directories that fail to parse are skipped. */
    fun list(): List<Project>

    /** Creates a new `.lscan` directory (with `streams/`, `streams/frames/`, `processed/`, `merged/`, `exports/`) and its manifest. */
    fun create(name: String, sensor: SensorType, profile: WorkflowProfile): Project

    /** Re-reads a project's manifest from disk by [id], or null if it no longer exists / fails to parse. */
    fun open(id: String): Project?

    /** Recursively deletes the project directory. Returns false if it didn't exist. */
    fun delete(id: String): Boolean

    /**
     * Read-modify-write of a project's `manifest.json`. Returns the updated
     * project, or null if the project no longer exists.
     *
     * Added by B7, which is the first task that has to *mutate* a manifest
     * (the mount calibration, Tech Spec §3.11's "mount calib" field). B1
     * deliberately shipped a create-only store; every later field —
     * `pointCountEstimate` on capture stop (still unwired, see NOTES.md),
     * `crsEpsg` from A10 — needs exactly this, so it belongs on the interface
     * rather than as a B7-local helper.
     *
     * Not atomic against a concurrent writer in another process: there is
     * exactly one process writing a project's manifest, and the alternative
     * (a lock file per project) would buy nothing here.
     */
    fun updateManifest(id: String, transform: (ProjectManifest) -> ProjectManifest): Project?
}
