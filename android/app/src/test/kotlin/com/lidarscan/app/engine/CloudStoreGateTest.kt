package com.lidarscan.app.engine

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 39 item 191 — the two properties the round-38 crash needed and had
 * neither of: a clear cannot run while a reader is reading, and a reader can
 * tell that a clear has happened.
 */
class CloudStoreGateTest {

    @Test
    fun theOpenGateJustRunsTheBlock() {
        assertEquals(7, CloudStoreGate.OPEN.read { 7 })
        assertEquals(9, CloudStoreGate.OPEN.mutate { 9 })
        assertEquals(0L, CloudStoreGate.OPEN.epoch)
    }

    @Test
    fun everyMutationIsANewEpoch() {
        val gate = ReadWriteCloudStoreGate()
        val first = gate.epoch
        gate.mutate { }
        val second = gate.epoch
        gate.mutate { }
        assertNotEquals("a clear must be observable", first, second)
        assertNotEquals(second, gate.epoch)
        assertTrue("epochs only ever go forward", gate.epoch > first)
    }

    /**
     * The epoch is bumped INSIDE the exclusive window, so no reader can ever
     * see a store that has been cleared still wearing the epoch it had before —
     * which is the read that would make a renderer keep the freed generation's
     * pages.
     */
    @Test
    fun aReaderNeverSeesAClearedStoreWearingTheOldEpoch() {
        val gate = ReadWriteCloudStoreGate()
        val before = gate.epoch
        val cleared = AtomicBoolean(false)
        val inMutate = CountDownLatch(1)
        val releaseMutate = CountDownLatch(1)
        val seenEpoch = AtomicLong(-1)
        val seenCleared = AtomicBoolean(false)

        val writer = Thread {
            gate.mutate {
                cleared.set(true)
                inMutate.countDown()
                releaseMutate.await(5, TimeUnit.SECONDS)
            }
        }
        writer.start()
        assertTrue(inMutate.await(5, TimeUnit.SECONDS))

        val reader = Thread {
            gate.read {
                seenCleared.set(cleared.get())
                seenEpoch.set(gate.epoch)
            }
        }
        reader.start()
        // The reader is queued behind the writer: it cannot have run yet.
        Thread.sleep(150)
        assertEquals("the reader got in during a clear", -1L, seenEpoch.get())

        releaseMutate.countDown()
        writer.join(5_000)
        reader.join(5_000)

        assertTrue("the reader ran after the clear", seenCleared.get())
        assertNotEquals("…and saw the new epoch", before, seenEpoch.get())
    }

    /** Readers do not exclude each other — the render thread and a measure pick overlap. */
    @Test
    fun readersRunTogether() {
        val gate = ReadWriteCloudStoreGate()
        val bothIn = CountDownLatch(2)
        val done = CountDownLatch(2)
        val overlapped = AtomicBoolean(false)
        repeat(2) {
            Thread {
                gate.read {
                    bothIn.countDown()
                    if (bothIn.await(5, TimeUnit.SECONDS)) overlapped.set(true)
                }
                done.countDown()
            }.start()
        }
        assertTrue(done.await(10, TimeUnit.SECONDS))
        assertTrue("two readers must be able to hold the gate at once", overlapped.get())
    }

    /** And a clear waits for the reader that is already inside. */
    @Test
    fun aClearWaitsForTheReaderItWouldHaveFreedUnder() {
        val gate = ReadWriteCloudStoreGate()
        val readerIn = CountDownLatch(1)
        val releaseReader = CountDownLatch(1)
        val clearedWhileReading = AtomicBoolean(false)
        val readerDone = AtomicBoolean(false)

        val reader = Thread {
            gate.read {
                readerIn.countDown()
                releaseReader.await(5, TimeUnit.SECONDS)
                readerDone.set(true)
            }
        }
        reader.start()
        assertTrue(readerIn.await(5, TimeUnit.SECONDS))

        val writer = Thread { gate.mutate { clearedWhileReading.set(!readerDone.get()) } }
        writer.start()
        Thread.sleep(150)
        assertFalse("the clear ran while a reader held the gate", clearedWhileReading.get())

        releaseReader.countDown()
        reader.join(5_000)
        writer.join(5_000)
        assertFalse("the clear must have waited for the reader", clearedWhileReading.get())
    }
}
