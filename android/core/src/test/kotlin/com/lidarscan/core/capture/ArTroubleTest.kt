package com.lidarscan.core.capture

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 27 item 142 — the first outside user could not scan, six times, and the
 * screen never changed.
 *
 * The device that reproduces it (an OPPO CPH2499 whose ColorOS power manager
 * takes the camera back from ARCore) is not on this desk and never will be, so
 * every claim about what the app SAYS in that state has to be a unit test. The
 * ones below are the three that matter: the persistence threshold (a blink is
 * not a fault), the ordering (a missing APK explains everything downstream of
 * it), and the refusal to offer a retry that must fail.
 */
class ArTroubleTest {

    private fun kind(
        ready: Boolean = true,
        needsInstall: Boolean = false,
        unsupported: Boolean = false,
        fatalSince: Long? = null,
        now: Long = 10_000L,
    ) = ArTrouble.kindFor(ready, needsInstall, unsupported, fatalSince, now)

    @Test
    fun `a healthy tracker says nothing`() {
        assertEquals(ArTroubleKind.NONE, kind())
    }

    @Test
    fun `a blink is not a fault`() {
        // 2.9 s of failure: ARCore does go quiet through a hard turn, and round
        // 16's watchdog already paid for the lesson that a banner which cries
        // wolf on an ordinary corner is one the operator stops reading.
        assertEquals(ArTroubleKind.NONE, kind(fatalSince = 10_000L - 2_900L))
    }

    @Test
    fun `three seconds of a dead camera IS a fault`() {
        assertEquals(
            ArTroubleKind.CAMERA_STOPPED,
            kind(fatalSince = 10_000L - ArTrouble.FATAL_PERSIST_MILLIS),
        )
    }

    @Test
    fun `a missing ARCore APK is reported instead of the camera`() {
        // The OPPO's cousin: telling somebody whose ARCore is not installed
        // that their "tracking camera stopped" sends them to a battery setting
        // for a problem the Play Store fixes.
        assertEquals(
            ArTroubleKind.NEEDS_INSTALL,
            kind(ready = false, needsInstall = true, fatalSince = 0L),
        )
    }

    @Test
    fun `an unsupported device is told the truth, once`() {
        assertEquals(
            ArTroubleKind.UNSUPPORTED,
            kind(ready = false, unsupported = true, fatalSince = 0L),
        )
        assertFalse(
            "a Retry on a device that cannot run ARCore is a promise the app cannot keep",
            ArTrouble.retryable(ArTroubleKind.UNSUPPORTED),
        )
    }

    @Test
    fun `a device still CHECKING is not yet a fault`() {
        assertEquals(ArTroubleKind.NONE, kind(ready = false, fatalSince = 0L))
    }

    @Test
    fun `only the camera fault offers a retry`() {
        assertTrue(ArTrouble.retryable(ArTroubleKind.CAMERA_STOPPED))
        assertFalse(ArTrouble.retryable(ArTroubleKind.NEEDS_INSTALL))
        assertFalse(ArTrouble.retryable(ArTroubleKind.NONE))
    }

    @Test
    fun `every state that is a fault has words, and NONE has none`() {
        assertNull(ArTrouble.title(ArTroubleKind.NONE))
        assertNull(ArTrouble.detail(ArTroubleKind.NONE))
        for (k in ArTroubleKind.entries.filter { it != ArTroubleKind.NONE }) {
            assertTrue("$k needs a headline", !ArTrouble.title(k).isNullOrBlank())
            assertTrue("$k needs its one detail line", !ArTrouble.detail(k).isNullOrBlank())
        }
    }

    /** Item 98's law, on the strings this item adds. */
    @Test
    fun `the wording obeys the law`() {
        ArTrouble.INSTRUCTIONS.forEach {
            assertTrue(
                "instruction is ${WordingLaw.wordCount(it)} words: \"$it\"",
                WordingLaw.isInstruction(it),
            )
        }
        ArTrouble.DETAILS.forEach {
            assertTrue("detail is ${WordingLaw.wordCount(it)} words: \"$it\"", WordingLaw.isDetail(it))
        }
        (ArTrouble.INSTRUCTIONS + ArTrouble.DETAILS).forEach {
            assertTrue("jargon ${WordingLaw.jargonIn(it)} in \"$it\"", WordingLaw.jargonIn(it).isEmpty())
        }
    }
}
