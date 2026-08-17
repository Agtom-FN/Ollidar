package com.lidarscan.core.capture

import java.time.ZoneId
import java.time.ZonedDateTime
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Round 5 item 9: the auto-naming series logic. Pure JVM — no emulator, no
 * DataStore (the counter's *persistence* is `:app`'s SettingsRepository; the
 * *format* is this).
 */
class ScanAutoNameTest {

    private val utc = ZoneId.of("UTC")

    /** 2026-08-17 19:32:07 UTC — the owner's own example timestamp. */
    private val ownerExampleMillis =
        ZonedDateTime.of(2026, 8, 17, 19, 32, 7, 0, utc).toInstant().toEpochMilli()

    @Test
    fun `formats the owner's example as a filesystem-safe name`() {
        assertEquals("Scan-014-2026-08-17-1932", ScanAutoName.format(14, ownerExampleMillis, utc))
    }

    @Test
    fun `series is zero-padded to three digits`() {
        assertEquals("Scan-001-2026-08-17-1932", ScanAutoName.format(1, ownerExampleMillis, utc))
        assertEquals("Scan-099-2026-08-17-1932", ScanAutoName.format(99, ownerExampleMillis, utc))
        assertEquals("Scan-999-2026-08-17-1932", ScanAutoName.format(999, ownerExampleMillis, utc))
    }

    @Test
    fun `series past 999 grows rather than wrapping or truncating`() {
        assertEquals("Scan-1000-2026-08-17-1932", ScanAutoName.format(1000, ownerExampleMillis, utc))
        assertEquals("Scan-12345-2026-08-17-1932", ScanAutoName.format(12345, ownerExampleMillis, utc))
    }

    @Test
    fun `a zero or negative counter still names a usable project`() {
        assertEquals("Scan-001-2026-08-17-1932", ScanAutoName.format(0, ownerExampleMillis, utc))
        assertEquals("Scan-001-2026-08-17-1932", ScanAutoName.format(-7, ownerExampleMillis, utc))
    }

    @Test
    fun `date and time components are zero-padded`() {
        val millis = ZonedDateTime.of(2027, 1, 2, 3, 4, 0, 0, utc).toInstant().toEpochMilli()
        assertEquals("Scan-005-2027-01-02-0304", ScanAutoName.format(5, millis, utc))
    }

    @Test
    fun `the auto name contains no character that is illegal on exFAT or NTFS`() {
        val name = ScanAutoName.format(14, ownerExampleMillis, utc)
        for (c in charArrayOf('\\', '/', ':', '*', '?', '"', '<', '>', '|', ' ')) {
            assertFalse("auto name must not contain '$c': $name", name.contains(c))
        }
    }

    @Test
    fun `resolve prefers a typed name`() {
        assertEquals("Warehouse bay 3", ScanAutoName.resolve("  Warehouse bay 3 ", 14, ownerExampleMillis, utc))
    }

    @Test
    fun `resolve falls back to the auto name for blank input`() {
        assertEquals("Scan-014-2026-08-17-1932", ScanAutoName.resolve(null, 14, ownerExampleMillis, utc))
        assertEquals("Scan-014-2026-08-17-1932", ScanAutoName.resolve("", 14, ownerExampleMillis, utc))
        assertEquals("Scan-014-2026-08-17-1932", ScanAutoName.resolve("   ", 14, ownerExampleMillis, utc))
    }

    @Test
    fun `resolve falls back when a typed name sanitizes to nothing`() {
        // "//" is a path, not a name: sanitizing leaves an empty string, and a
        // project called "" (or "-", after the store slugifies it) is worse than
        // the auto name.
        assertEquals("Scan-014-2026-08-17-1932", ScanAutoName.resolve("//", 14, ownerExampleMillis, utc))
        assertEquals("Scan-014-2026-08-17-1932", ScanAutoName.resolve("::?*", 14, ownerExampleMillis, utc))
    }

    @Test
    fun `sanitize strips path separators and windows-illegal characters`() {
        assertEquals("etc passwd", ScanAutoName.sanitize("/etc/passwd"))
        assertEquals("C Site A", ScanAutoName.sanitize("C:\\Site A"))
        assertEquals("bay 3", ScanAutoName.sanitize("bay?3"))
        assertTrue(ScanAutoName.sanitize("a<b>c|d\"e").none { it in charArrayOf('<', '>', '|', '"') })
    }

    @Test
    fun `sanitize collapses whitespace, drops control characters and trailing dots`() {
        assertEquals("north wing", ScanAutoName.sanitize("north\t\n   wing"))
        assertEquals("scan", ScanAutoName.sanitize("scan\u0000\u0007"))
        // A trailing dot is legal on ext4 and silently dropped by Windows, which
        // is exactly the kind of name that round-trips differently on the two
        // platforms a .lscan travels between.
        assertEquals("site", ScanAutoName.sanitize("site..."))
    }
}
