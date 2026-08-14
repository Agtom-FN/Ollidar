package com.lidarscan.core.store

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
}
