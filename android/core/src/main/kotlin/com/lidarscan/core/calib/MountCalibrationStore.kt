package com.lidarscan.core.calib

import java.io.File
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json

/**
 * The **device-level** calibration store WIZARD.md §3 asks for:
 *
 * > Calibration belongs to the bracket, not the project. Keep a device-level
 * > store keyed by (phone model, bracket ID, lidar serial) and offer it as the
 * > default for new projects — the user should calibrate once, not once per
 * > scan.
 *
 * One JSON file, whole-file rewrite on every save. That is the right shape at
 * this size (a handful of records, written once per calibration, read once
 * per capture start) and it makes the "written atomically" property trivial:
 * write a sibling `.tmp` and rename, so a kill mid-write leaves the previous
 * file intact rather than a truncated one.
 *
 * The per-project copy lives in `manifest.json`
 * ([com.lidarscan.core.model.ProjectManifest.mountCalibration]) and is
 * written separately — a `.lscan` must stay self-describing when it is moved
 * to a desktop that has never seen this phone.
 */
interface MountCalibrationStore {
    fun all(): List<MountCalibration>
    fun find(phoneModel: String, bracketId: String, sensorSerial: String?): MountCalibration?
    fun save(calibration: MountCalibration)
    fun delete(id: String)
}

class FileMountCalibrationStore(private val file: File) : MountCalibrationStore {

    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    private fun load(): MutableList<MountCalibration> {
        if (!file.exists()) return mutableListOf()
        return try {
            json.decodeFromString(ListSerializer(MountCalibration.serializer()), file.readText())
                .toMutableList()
        } catch (e: Exception) {
            // A corrupt store is not a reason to refuse to run: the wizard can
            // always re-derive a calibration, and a hard failure here would
            // block capture entirely. Same posture as FileProjectStore's
            // corrupt-manifest skip.
            mutableListOf()
        }
    }

    private fun persist(records: List<MountCalibration>) {
        file.parentFile?.mkdirs()
        val tmp = File(file.parentFile, "${file.name}.tmp")
        tmp.writeText(json.encodeToString(ListSerializer(MountCalibration.serializer()), records))
        if (!tmp.renameTo(file)) {
            file.delete()
            tmp.renameTo(file)
        }
    }

    override fun all(): List<MountCalibration> = load().sortedByDescending { it.createdAtEpochMillis }

    override fun find(phoneModel: String, bracketId: String, sensorSerial: String?): MountCalibration? {
        val key = MountCalibration.storeKey(phoneModel, bracketId, sensorSerial)
        val records = load()
        // Exact key first; then the same phone+bracket with any serial, since
        // a D6 reports no serial at all over its UART and a Mid-360 swapped
        // for an identical unit on the same bracket is still a better default
        // than nothing (the wizard's re-check step is what catches a mount
        // that actually moved).
        return records.firstOrNull { it.storeKey() == key }
            ?: records.filter { it.phoneModel == phoneModel && it.bracketId == bracketId }
                .maxByOrNull { it.createdAtEpochMillis }
    }

    override fun save(calibration: MountCalibration) {
        val records = load()
        records.removeAll { it.id == calibration.id || it.storeKey() == calibration.storeKey() }
        records.add(calibration)
        persist(records)
    }

    override fun delete(id: String) {
        val records = load()
        if (records.removeAll { it.id == id }) persist(records)
    }
}
